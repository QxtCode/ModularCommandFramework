#include"Friend_PM.h"
#include"Print_Module.h"

bool PrintFriend::Bind(PrintModule* module)
{
	if (module==nullptr)return false;

	in_module = module;
	RegisterAllFunctions();
	return true;
}

void PrintFriend::RegisterAllFunctions()
{
	//说明区域成员给lambda表达室捕获模块的指针
	PrintModule* module_this = this->in_module;

	//打印版本号和模块名称
	in_module->RegisterFunc("1", "打印", [module_this](ParmarPack& pack) {
		std::cout << module_this->version_ << " | " << module_this->GetName() << std::endl;
		pack.success = true;
		pack.return_value = "module_Versions_:"+module_this->version_;
		return;
		});

	in_module->RegisterFunc("2", "打印自定义字符", [module_this](ParmarPack& pack) {
		
		//临时解析包内容
		auto P_key = pack.pramer_list.find(ParamKeyToStr(ParamKey::PARAM));
		//auto v_size = P_key->second.size();
		if (P_key != pack.pramer_list.end())
		{
			for (auto& v : P_key->second)
			{
				/*module_this->print(v);*/
				std::cout << v << std::endl;
			}
		}

		/*module_this->print();*/
		return;
		});


}

