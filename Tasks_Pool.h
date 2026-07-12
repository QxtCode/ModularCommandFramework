#pragma once
#include"Task.h"
#include<vector>
#include<thread>
#include<list>
/*
	========说明=========
	1.这是一个任务类型池：
	·一个类都是一个任务
	·独立于命令类
	2.工作流程：
	·TasksPool初始化时已经分配好了任务列表的大小
	·使用CreateTask()函数将task类初始化，存入task_list_
	·在委托类的调用GetTaskPtr下拿到task的原始指针
	 
	
*/
class ModuleLifeManager;
class TasksPool
{
public:
	TasksPool(size_t MaxTask) 
	{
		task_list_.reserve(MaxTask);
	};
	~TasksPool() = default;

	//初始化任务
	Task* CreateTask(std::unique_ptr<ParmarPack>pack, ModuleLifeManager& life);
	

	//检查任务状态成功并重置任务
	//返回一个空闲任务容器的引索
	size_t CheckTask();

	// 禁用拷贝
	TasksPool(const TasksPool&) = delete;
	TasksPool& operator=(const TasksPool&) = delete;

	
private:
	std::vector<std::unique_ptr<Task>> task_list_;//任务数组
	std::list<size_t> free_task_;//空闲的任务引索
	std::thread check_th_;//检查线程
	
};