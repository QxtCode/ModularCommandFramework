#pragma once
/* 
	1.这是一个打印模块
	2.功能模块的职责是查表驱动功能
	3.继承一个功能模块的基类
*/
#include<iostream>
#include<unordered_map>
#include<string>
#include<functional>
#include"Moduel_Base_Object.h"

class PrintModule:public ModuleBaseObject
{
	//using PrintFunc = std::function<void(const std::string &param)>;
	using PrintFunc = std::function<void(ParmarPack&)>;
	friend class PrintFriend;
public:
	
	inline void  print(const std::string &s)
	{
		std::cout << s << std::endl;
	}

	/// <summary>
	/// 注册函数
	/// </summary>
	/// <param name="func_id">函数id</param>
	/// <param name="definition">说明</param>
	/// <param name="PrintFunc">函数体</param>
	void RegisterFunc(const std::string& func_id, const std::string& definition, const PrintFunc& print_func);

	/*/// <summary>
	/// 注册函数（无参数）
	/// </summary>
	/// <param name="func_id">函数id</param>
	/// <param name="definition">说明</param>
	/// <param name="print_func">函数体（无参数）</param>
	void No_Param_RegisterFunc(const std::string& func_id, const std::string& definition, const PrintFunc& print_func);*/

	/// <summary>
	/// 调用与指定函数 ID 关联的函数指针，并传递参数（如果存在）。
	/// </summary>
	/// <param name="pack">包含函数 ID 和参数列表的 ParmarPack 对象。</param>
	inline void CallFunc(ParmarPack* pack)
	{
		auto func_ptr=funcs_.find(pack->func_id);
		if (func_ptr == funcs_.end())
		{	
			pack->success = false;
			pack->error.code = 50;
			pack->error.message = "未找到函数id为:" + pack->func_id + " 的函数";
			std::cout << pack->error.code << "\n" << pack->error.message;
			return;
		}
		if (!pack->pramer_list.empty()) 
		{
			//auto key = pack.ParamKeyToStr(ParamKey::PARMR);
			//auto it = pack.pramer_list.find(key);
			//if (it != pack.pramer_list.end() && !it->second.empty())
			//{
			//	func_ptr->second(it->second.back()); // 传递第一个参数
			//}
			func_ptr->second(*pack);
		}
	}

	/// <summary>
	/// 打印模块功能 （后期基类做接口）
	/// </summary>
 inline	void Help(ParmarPack* pack) override
	{
		for (const auto &exper : explains_)
		{
			std::cout << exper.first << " : " << exper.second << std::endl;
		}
	}

	/// 实现基类接口：执行命令（查函数表 → 调用对应函数）
	inline void Execute(ParmarPack* pack) override
	{
		CallFunc(pack);
	}
 
 inline void ReturnValue(ParmarPack* pack)override
 {

 }

 inline const char* GetName() const override
 {
	 return "PrintModule";
 }
private:
	std::unordered_map<std::string, PrintFunc> funcs_;//函数表(funcs_id + funcs)
	std::unordered_map<std::string, std::string> explains_;//函数说明表(funcs_id + explain )
	int version_ = 1;
	
};