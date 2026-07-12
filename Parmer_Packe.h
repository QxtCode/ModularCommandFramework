#pragma once
#include<vector>
#include<unordered_map>
#include<string>

//参数包关键字
enum class ParamKey
{
	MSG,
	VALUE,
	AUTHOR,
	PARAM
};

/// <summary>
	/// 将枚举值 ParamKey 转换为对应的字符串表示。
	/// </summary>
	/// <param name="key">要转换为字符串的 ParamKey 枚举值。</param>
	/// <returns>与给定 ParamKey 枚举值对应的字符串。如果没有匹配项，则返回空字符串。</returns>
inline const char* ParamKeyToStr(const ParamKey& key)
{
	switch (key) {
	case ParamKey::MSG: return "msg";
	case ParamKey::VALUE: return "value";
	case ParamKey::AUTHOR: return "author";
	case ParamKey::PARAM:return "Param";

	default: return "";
	}
}

//参数包（包含错误error、返回值）
class ParmarPack
{
public:
	ParmarPack() = default;
	~ParmarPack() = default;
	//模块id（命令）
	std::string mod_id;

	//函数id
	std::string func_id;

	//参数列表
	std::unordered_map<std::string, std::vector<std::string>> pramer_list;

	// 执行是否成功
	bool success=false;

	// 返回值（支持多种类型）
	std::string return_value;

	//错误信息
	struct Error
	{
		int code = 0;//错误码
		std::string message;//错误信息
	}error;
	
};