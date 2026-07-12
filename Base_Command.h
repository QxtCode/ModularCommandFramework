#pragma once
#include<unordered_map>
#include<vector>
#include"Parmer_Packe.h"
#include"Moduel_Base_Object.h"
class BaseCommand
{
public:

	
	/// <summary>
	/// 帮助
	/// </summary>
	virtual void Help() = 0;

	/// <summary>
	/// 传信函数
	/// </summary>
	virtual void Execute(ParmarPack *pack) = 0;

	/// <summary>
	/// 挂载返回值
	/// </summary>
	virtual void Result() = 0;
};