#pragma once
#include <iostream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include "ModuleBaseObject.h"
#include "ParmarPack.h"
#include "event_bus/event_bus.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "modules/logging/LogModule.h"

// =================================================================
//  ModuleLifeManager — module lifecycle manager (singleton)
// =================================================================
//
//  v2.4 changes:
//    - module_map_ uses shared_ptr instead of unique_ptr.
//      EventBus slots hold weak_ptr to the module → if the module
//      is unloaded while a slot is in-flight, the slot detects
//      IsExpired() and skips execution safely.
//    - AddModule fixed: TOCTOU eliminated by holding unique_lock
//      for the entire check+insert operation.
//    - Slot lambdas no longer capture raw [this] — they capture
//      a shared_ptr, and the slot's WeakRefHolder checks liveness
//      before each call.
// =================================================================

class ModuleLifeManager
{
public:
    static ModuleLifeManager& GetInstance()
    {
        static ModuleLifeManager instance;
        return instance;
    }

    // ============================================================
    //  Register a built-in module
    // ============================================================
    /// v2.4: unique_lock held for the entire operation — no TOCTOU.
    /// Module is converted to shared_ptr so EventBus slots can use
    /// weak_ptr lifetime checking.
    bool AddModule(std::unique_ptr<ModuleBaseObject> module)
    {
        if (!module) return false;

        const char* name = module->GetName();

        // ---- OnInit (safe outside lock — only touches funcs_ table) ----
        if (!module->OnInit())
        {
            std::cerr << "[ModuleMgr] " << name << " OnInit() failed.\n";
            return false;
        }

        // ---- Convert to shared_ptr for weak_ptr lifetime tracking ----
        auto shared_mod = std::shared_ptr<ModuleBaseObject>(std::move(module));

        // ---- unique_lock for the entire check+insert (no TOCTOU) ----
        std::unique_lock wr_lock(mutex_);

        if (module_map_.find(name) != module_map_.end())
        {
            std::cerr << "[ModuleMgr] Module '" << name << "' already registered.\n";
            return false;  // shared_mod destroyed → module deleted
        }

        // ---- Connect to EventBus with shared_ptr (weak_ref protection) ----
        shared_mod->ConnectToEventBus(shared_mod);

        std::cout << "[ModuleMgr] Module loaded: " << name
                  << " v" << shared_mod->GetVersion() << "\n";
        LOG_INFO(std::string("Module loaded: ") + name +
                 " v" + std::to_string(shared_mod->GetVersion()));

        // ---- Auto-register "help" signal ----
        auto& bus = EventBus::GetInstance();
        std::string help_sig = std::string(name) + ".help";
        bus.RegisterSignal<ParmarPack*>(help_sig);
        bus.LinkSlotFunc<ParmarPack*>(help_sig, shared_mod,
            [raw = shared_mod.get()](ParmarPack* p) {
                raw->Help(p); p->success = true;
            });

        module_map_[name] = shared_mod;
        return true;
    }

    // ============================================================
    //  Direct dispatch (bypasses EventBus for backward compat)
    // ============================================================
    bool Dispatch(ParmarPack* pack)
    {
        if (!pack || pack->mod_id.empty())
        {
            PrintModuleList();
            return pack != nullptr;
        }

        if (pack->func_id == "help" || pack->mod_id == "help")
        {
            if (pack->mod_id == "help")
            {
                PrintModuleList();
                return true;
            }
        }

        std::shared_lock lock(mutex_);
        auto it = module_map_.find(pack->mod_id);
        if (it == module_map_.end())
        {
            pack->success = false;
            pack->error.code = 404;
            pack->error.message = "Module not found: " + pack->mod_id;
            return false;
        }

        if (pack->func_id == "help")
            it->second->Help(pack);
        else
            it->second->Execute(pack);

        return true;
    }

    // ============================================================
    //  Module query
    // ============================================================
    void PrintModuleList() const
    {
        std::shared_lock lock(mutex_);
        LOG_PLAIN("=== Registered Modules ===");
        for (const auto& [name, m] : module_map_)
            LOG_PLAIN("  " << name << " v" << m->GetVersion());
    }

    size_t GetModuleCount() const
    {
        std::shared_lock lock(mutex_);
        return module_map_.size();
    }

    ModuleBaseObject* GetModule(const std::string& name)
    {
        std::shared_lock lock(mutex_);
        auto it = module_map_.find(name);
        return (it != module_map_.end()) ? it->second.get() : nullptr;
    }

    // ============================================================
    //  Unload module (built-in and DLL plugins)
    // ============================================================
    /// v2.4: Step-by-step safe shutdown:
    ///   1. OnShutdown() — module's cleanup hook
    ///   2. RemoveSignal() for all module signals — blocks until all
    ///      in-flight Emit() calls finish (bus_mutex_ shared_lock vs
    ///      RemoveSignal's unique_lock)
    ///   3. module_map_.erase() — drops shared_ptr ref count
    ///      If in-flight Slot::Run() holds a Lock()'d shared_ptr,
    ///      the module object stays alive until Run() returns.
    ///   4. FreeLibrary() — only AFTER all slots released their
    ///      shared_ptr, the module object is fully destroyed and
    ///      DLL code is safe to unmap.
    bool UnloadModule(const std::string& name)
    {
        std::unique_lock lock(mutex_);
        auto it = module_map_.find(name);
        if (it == module_map_.end()) return false;

        it->second->OnShutdown();

        // Remove ALL signals for this module from EventBus
        {
            std::string prefix = name + ".";
            auto& bus = EventBus::GetInstance();
            for (const auto& sig : bus.GetSignalNames())
            {
                if (sig.compare(0, prefix.size(), prefix) == 0)
                    bus.RemoveSignal(sig);
            }
        }

        module_map_.erase(it);  // drops shared_ptr ref → may delete module

        // FreeLibrary if this was a DLL module
        auto dll = dll_handles_.find(name);
        if (dll != dll_handles_.end())
        {
            FreeLibrary(dll->second);
            dll_handles_.erase(dll);
        }

        std::cout << "[ModuleMgr] Module unloaded: " << name << "\n";
        LOG_INFO(std::string("Module unloaded: ") + name);
        return true;
    }

    // ============================================================
    //  DLL hot-load
    // ============================================================
    using CreateFunc  = ModuleBaseObject* (*)();
    using DestroyFunc = void (*)(ModuleBaseObject*);

    bool LoadDLLModule(const std::string& dll_path)
    {
#ifdef _WIN32
        HMODULE handle = LoadLibraryA(dll_path.c_str());
        if (!handle)
        {
            std::cerr << "[ModuleMgr] Failed to load DLL: " << dll_path << "\n";
            LOG_ERROR(std::string("Failed to load DLL: ") + dll_path);
            return false;
        }

        auto create = reinterpret_cast<CreateFunc>(
            GetProcAddress(handle, "CreateModule"));
        if (!create)
        {
            std::cerr << "[ModuleMgr] DLL missing CreateModule: " << dll_path << "\n";
            FreeLibrary(handle);
            return false;
        }

        ModuleBaseObject* raw = create();
        if (!raw)
        {
            std::cerr << "[ModuleMgr] CreateModule() returned null.\n";
            FreeLibrary(handle);
            return false;
        }

        // Wrap in unique_ptr → AddModule converts to shared_ptr
        bool ok = AddModule(std::unique_ptr<ModuleBaseObject>(raw));
        if (ok)
        {
            dll_handles_[raw->GetName()] = handle;
            return true;
        }

        // AddModule failed — module already deleted by unique_ptr destructor
        FreeLibrary(handle);
        return false;
#else
        std::cerr << "[ModuleMgr] DLL loading only supported on Windows.\n";
        return false;
#endif
    }

    // ============================================================
    //  Scan plugin directory
    // ============================================================
    int ScanPluginDirectory(const std::string& dir_path)
    {
        int loaded = 0;

#ifdef _WIN32
        std::string search = dir_path + "\\*.dll";

        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(search.c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) return 0;

        do
        {
            std::string dll_path = dir_path + "\\" + fd.cFileName;
            if (LoadDLLModule(dll_path))
                loaded++;
        }
        while (FindNextFileA(hFind, &fd));

        FindClose(hFind);
#else
        std::cerr << "[ModuleMgr] Plugin scanning only supported on Windows.\n";
#endif

        std::cout << "[ModuleMgr] Plugins loaded: " << loaded << "\n";
        return loaded;
    }

    ModuleLifeManager(const ModuleLifeManager&) = delete;
    ModuleLifeManager& operator=(const ModuleLifeManager&) = delete;
    ModuleLifeManager(ModuleLifeManager&&) = delete;
    ModuleLifeManager& operator=(ModuleLifeManager&&) = delete;

private:
    ModuleLifeManager() = default;

    // v2.4: shared_ptr so EventBus slots can hold weak_ptr for lifetime checks
    std::unordered_map<std::string, std::shared_ptr<ModuleBaseObject>> module_map_;
    std::unordered_map<std::string, HMODULE> dll_handles_;
    mutable std::shared_mutex mutex_;
};

// =================================================================
//  EXPORT_MODULE macro — DLL export interface
// =================================================================
//
//  Usage: put EXPORT_MODULE(YourClass) at the end of your module .cpp
//
//  CreateModule() is called by LoadDLLModule to instantiate the module.
//  DestroyModule() is DEPRECATED in v2.4 — modules are now managed by
//  shared_ptr, which calls the default deleter (delete). Calling
//  DestroyModule on a shared_ptr-managed module would double-delete.
//  It is kept for backward compatibility with external users who may
//  manage module lifetime manually.
#define EXPORT_MODULE(ClassName)                                              \
    extern "C" __declspec(dllexport) ModuleBaseObject* CreateModule() {       \
        return new ClassName();                                               \
    }                                                                         \
    extern "C" __declspec(dllexport) void DestroyModule(ModuleBaseObject* m) {\
        if (m) { m->OnShutdown(); delete m; } /* DEPRECATED in v2.4 */        \
    }
