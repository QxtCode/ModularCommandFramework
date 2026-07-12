#pragma once
#include"Command_Parser_Base.h"
#include <unordered_map>
#include<functional>
#include<sstream>

 inline std::string RemoveSpace(std::string str){
	auto new_end = std::remove(str.begin(), str.end(), ' ');
	str.erase(new_end, str.end());
	return str;
}


class ConsoleParser:public CommandParserBase
{
public:
	using FormatFunc = std::function<void(std::any)>;
	using CmdWriteFunc = std::function<void(ParmarPack*,const std::string&)>;
	ConsoleParser();
	~ConsoleParser() = default;

	static ConsoleParser &getParser()
	{
		static ConsoleParser console;
		return console;
	}

	std::unique_ptr<ParmarPack>PopPack()override;

	//注册解析函数
	void Register(const std::string&flag, FormatFunc format_func);

	//接收参数
	int sendCommand(const std::string& flag, std::any value) override;

	//接收类型
	const std::type_info& AcceptedType() override;

	//注册解析头
	void RegisterCmdHead(const std::string& head, CmdWriteFunc writefunc);

	//禁止拷贝
	ConsoleParser(const ConsoleParser& other) = delete;
	ConsoleParser& operator=(const ConsoleParser& other) = delete;
	//禁止移动
	ConsoleParser(ConsoleParser&& other) = delete;
	ConsoleParser& operator=(ConsoleParser&& other) = delete;



	
private:
	//插入参数包
	void PushPack(std::unique_ptr<ParmarPack> pack)override;
	std::unordered_map<std::string, FormatFunc> fmt_;
	std::unordered_map<std::string, CmdWriteFunc> head_list_;
};