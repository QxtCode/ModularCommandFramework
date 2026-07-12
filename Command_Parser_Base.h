#pragma once
#include "Parmer_Packe.h"
#include "Lock_Queue.h"
#include<any>
//
//==========说明============
//	     解析基类
//	
//		

class CommandParserBase
{
public:
	CommandParserBase() = default;
	virtual ~CommandParserBase()=default;
	virtual std::unique_ptr<ParmarPack>PopPack() = 0;
protected:
	virtual void PushPack(std::unique_ptr<ParmarPack> pack) = 0;
	virtual int sendCommand(const std::string& flag, std::any value) = 0;
	virtual const std::type_info& AcceptedType() = 0;
protected:
	LockQueue<ParmarPack> queue_;
};