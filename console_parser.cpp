#include"Console_parser.h"
#include<cctype>
void ConsoleParser::PushPack(std::unique_ptr<ParmarPack> pack)
{
	if (pack) queue_.push(std::move(pack));
	
}

ConsoleParser::ConsoleParser()
{

	this->RegisterCmdHead("-m:", [](ParmarPack* pack, const std::string& value) {
		if (pack == nullptr)return;
		pack->mod_id = value;
		});
	this->RegisterCmdHead("-f:", [](ParmarPack* pack, const std::string& value) {
		if (pack == nullptr)return;
		pack->func_id = value;
		});
	this->RegisterCmdHead("-v:", [](ParmarPack* pack, const std::string& value)
		{
			if (pack == nullptr)return;
			std::string k, v;
			size_t start = 0;
			while (start < value.size()) {
				// 1. 找下一个逗号
				auto endindex = value.find(',', start);
				if (endindex == std::string::npos) {
					endindex = value.size(); // 没找到逗号，就一直到字符串末尾
				}

				// 2. 截取当前键值对
				std::string pair = value.substr(start, endindex - start);

				// 3. 用 | 分割键和值
				auto pipePos = pair.find('|');
				if (pipePos != std::string::npos) {
					k = pair.substr(0, pipePos);
					v = pair.substr(pipePos + 1);
					// 存入参数包
					pack->pramer_list[k].push_back(v);
				}
				else
				{
					std::cout << "no find '|' in str:" << pair << std::endl;
				}
				// 4. 移动 start 到下一个起始位置（跳过逗号）
				start = endindex + 1;
			}
		});
	Register("TXT", [this](std::any value) {
		auto& str = std::any_cast<std::string&>(value);
		//命令结构 -m:m. -f: -v:k|v,k|v,....
		auto pack = std::make_unique<ParmarPack>();

		std::stringstream iss(str);
		std::string pard;

		
		while (iss >> pard)
		{
			auto colonPos = pard.find(':');
			if (colonPos == std::string::npos)
			{
				std::cout << "cmd :" << pard << "erorr. no find ':'!" << std::endl;
				continue;
			}
			//命令头 -m -v.....
			auto cmdhead = pard.substr(0, colonPos+1);
			//命令的值
			auto cmdvalue = pard.substr(colonPos + 1);

			auto iter = head_list_.find(cmdhead);
			if (iter == head_list_.end())return;
			iter->second(pack.get(), cmdvalue);
		}
		//填入参数包
		this->PushPack(std::move(pack));
		});
}
/// <summary>
/// 命令输入，预设命令结构，-m:printmdoule -f:1 -v:key|value,key|value,key|value.........
/// </summary>
/// <param name="flag">命令格式</param>
/// <param name="value">命令参数（键值）</param>
/// <returns>int 成功1，否则-1</returns>
int ConsoleParser::sendCommand(const std::string& flag, std::any value)
{
	for (auto const& c : flag)
	{
		if (isspace(c))
		{
			std::cout << "flag contains space!" << std::endl;
			return -1;
		}
	}
	if (flag.empty() || !value.has_value())
	{
		std::cout << "flag is empty or value is null!" << std::endl;
		return -1;
	}
	if (value.type() != AcceptedType())
	{
		std::cout << "value type is ont string！" << std::endl;
		return -1;
	}

	//解析创建参数包
	auto const &iter=fmt_.find(flag);
	if (iter == fmt_.end())
	{
		std::cout << "ont have flag type" << std::endl;
		return -1;
	}
	iter->second(std::move(value));
	return 1;
}

std::unique_ptr<ParmarPack>ConsoleParser::PopPack(){
	return std::move(queue_.pop());
}

//注册解析函数
void ConsoleParser::Register(const std::string& flag, FormatFunc format_func){
	if (flag.empty() || format_func == nullptr)return;
	fmt_[RemoveSpace(flag)]= format_func;
}

//注册头函数
void ConsoleParser::RegisterCmdHead(const std::string& head, CmdWriteFunc writefunc){
	if (head.empty() || writefunc == nullptr)return;
	head_list_[RemoveSpace(head)] = writefunc;
}
//声明自己需要的对象
const std::type_info& ConsoleParser::AcceptedType(){
	return typeid(std::string);
}