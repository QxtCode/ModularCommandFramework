#include <iostream>
#include"Print_Module.h"
#include"Friend_PM.h"
#include"Command_Module.h"
#include"Parmer_Packe.h"
#include"Module_Life_Manager.h"
#include"Command_Parser_Base.h"
#include"Task.h"
#include"Console_parser.h"

/*
===========任务清单==========
- 一个命令式的插件架构
  1.核心三个功能点 
	·可以实现插件化热更新，模块自由替换
	·使用命令模式驱动，解耦层层转达指令
	·解析层shell，能解析以json、xml以及市场上的主流文本形式
  2.如何实现
	·解析层<shell>：以文件或网络拿到命令文本，或者以客户端前端按钮的指令获取文本形式，
		并解释其文本的格式以及命令参数

	·总命令层<Command_Manager>：**Command_Manager** 关联分命令层 **Command_Moduel**，
	    管理分发调度，写入模块名字主动锁定模块入口

	·分命令层<Command_Moduel>：是一层抽象层，分别持有所有模块的指针包含所有模块的基类
	    传递参数包调用模块基类的接口执行模块

	·模块层<Moduel>：模块层是功能实现的底层入口管理函数的挂载，被分命令层调动，
		模块内部自动查函数表主动执行函数基础 **ModuelBaseObject**模块基类
	
	·模块函数注册层<Moduel_fried>：函数注册不是在模块内执行的，是在外部注入实现，
		这层是模块的友好类型专门管理模块函数的注册

	·生命周期层<ModuleLife_Manager>：管理两大基类层，模块基类和分命令层基类，是这些类挂载的核心的周期维护者

*/


using namespace std;
int main()
{
	//==================测试==========================

	//1.初始化生命周期管理器
	   auto& LifeManager = ModuleLifeManager::GetInstance();
	   auto& consoleshell = ConsoleParser::getParser();
	   
	   //2. 创建打印模块，并用 PrintFriend 注册函数
		auto printModule = std::make_unique<PrintModule>();
		PrintFriend pf(printModule.get());   // 构造函数自动 Bind 并注册函数

		//3.创建命令管理模块，可以是框架内部完成创建
		auto cmdModule = std::make_unique<CommandModule>(printModule.get());

	   //4.注册模块
		LifeManager.AddModule(std::move(printModule));
		LifeManager.AddCommand("c_printmodule", std::move(cmdModule));

		auto a = consoleshell.sendCommand("TXT", string("-m:c_printmodule -f:2 -v:Param|cmd"));
		auto pack = consoleshell.PopPack();
		std::cout << pack->mod_id << " : " << pack->func_id <<" : " << *(pack->pramer_list.find(ParamKeyToStr(ParamKey::PARAM))->second.begin()) << endl;

			//6.执行任务
		Task task(std::move(pack), LifeManager);
		task.run();
		
		return 0;
}