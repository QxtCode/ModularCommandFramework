#include"Print_Module.h"
void PrintModule::RegisterFunc(const std::string &func_id, const std::string &definition, const PrintFunc& print_func)
{
	//注册入函数表
	if (func_id.empty() && definition.empty())return;
	funcs_[func_id] = print_func;
	
	//加入函数说明表
	explains_[func_id] = definition;
}


//void PrintModule::No_Param_Regs_Func(const std::string& func_id, const std::string& definition, const PrintFunc& print_func)
//{
//	//注册入函数表
//	if (func_id.empty() && definition.empty())return;
//	auto link_param = std::bind(print_func, std::string());
//	funcs_[func_id] = link_param;
//
//	//加入函数说明表
//	explains_[func_id] = definition;
//}

//void PrintModule::No_Param_Regs_Func(const std::string& func_id, const std::string& definition, const PrintFunc& print_func)
//{
//	// 注册入函数表
//	if (func_id.empty() && definition.empty()) return;
//
//	// 包装无参数函数，传递一个空的 ParmarPack
//	funcs_[func_id] = [print_func](ParmarPack& pack) {
//		ParmarPack empty_pack;
//		print_func(empty_pack);
//		};
//
//	// 加入函数说明表
//	explains_[func_id] = definition;
//}