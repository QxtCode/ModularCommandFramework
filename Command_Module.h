#pragma once
/*
	说明：
	1.这是命令模块
	2.一个模板类型负责传递参数调用功能模块查表
	3.继承一个命令基类负责一个功能模块

	职责：
	每一个命令模块，负责一个功能模块（解耦）
*/
#include "Base_Command.h"
    class CommandModule : public BaseCommand
    {
    public:
        CommandModule(ModuleBaseObject* module)
            :in_module(module) {}
        CommandModule() = default;

       //打印帮助
      inline  void Help() override
        {
            if (in_module!=nullptr) in_module->Help(nullptr);
        }

      //调用命令模块执行
      inline void Execute(ParmarPack* pack)override
        {
          if (!in_module) return;
          if (pack->func_id=="help"|| pack->func_id.empty())  return Help();
          in_module->Execute(pack);
        }
        
      //挂载模块函数执行的返回值
      inline  void Result() override
        {
            
        }

    private:
        ModuleBaseObject* in_module = nullptr;
    };
