#pragma once
/*
==========模块基类==========
  所有功能模块（包括 DLL 模块）都继承这个类

  生命周期：
    OnInit()     - 模块加载后调用，做初始化（打开文件/连接数据库等）
    Execute()    - 收到命令时调用
    OnShutdown() - 模块卸载前调用，做清理（关闭连接/释放资源等）

  使用方式（DLL 模块开发者）：
    class MyModule : public ModelBaseObject {
        const char* GetName() const override { return "MyModule"; }
        bool OnInit() override               { return true; }
        void Execute(ParmarPack*) override    {}
        void OnShutdown() override           {}
    };

    // DLL 导出工厂函数（必须在模块 cpp 里写这一行）
    EXPORT_MODULE(MyModule)
*/
#include <string>
#include <vector>
#include "Parmer_Packe.h"

class ModuleBaseObject
{
public:
    // ===== 虚析构（DLL 安全，必须写）=====
    virtual ~ModuleBaseObject() = default;

    // ===== 生命周期（有默认实现，子类按需重写）=====
    virtual bool OnInit()
    {
        return true;   // 加载成功返回 true，失败返回 false（模块不会被注册）
    }

    virtual void OnShutdown()
    {
        // 模块卸载时清理资源
    }

    // ===== 核心接口（必须重写）=====
    virtual void Execute(ParmarPack* pack) = 0;       // 执行命令
    virtual void Help(ParmarPack* pack) = 0;           // 返回帮助信息
    virtual void ReturnValue(ParmarPack* pack) = 0;    // 获取返回值

    // ===== 元信息（必须重写）=====
    virtual const char* GetName() const = 0;           // 模块唯一标识
    virtual int  GetVersion() const { return 1; }      // 版本号，热加载时判断兼容性
};
