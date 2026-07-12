#pragma once
/*
	这是打印模块的友元类
	负责跟打印模块注册本模块的私有成员操作
*/
#include<string>
class PrintModule;

class PrintFriend
{
public:
	PrintFriend(PrintModule * module) 
	{
		if (Bind(module))return;
	}

	PrintFriend() = default;//保留默认创建

	/// <summary>
	/// 绑定模块
	/// </summary>
	/// <param name="module">对应的模块</param>
	bool Bind(PrintModule* module);

	void RegisterAllFunctions();

private:
	PrintModule* in_module = nullptr;
	int Versions_ = 1;
	std::string Module_Name = "PrintModule";
};