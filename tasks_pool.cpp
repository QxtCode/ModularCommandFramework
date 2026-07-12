#include"Tasks_Pool.h"
#include<stdexcept>

//初始化任务
Task* TasksPool::CreateTask(std::unique_ptr<ParmarPack>pack, ModuleLifeManager& life)
{
	if (!pack) 
		return nullptr; 
	if (!free_task_.empty()){
		size_t index = free_task_.front();
		free_task_.pop_front();

		if (task_list_[index]->SetPack(std::move(pack)) != nullptr)
			return task_list_[index].get();
		else
		{
			throw std::runtime_error("The pack_ptr in the current task is empty !");
		}
	}
	// 2. 池子没满，新建并添加
	if (task_list_.size() < task_list_.capacity()) {
		task_list_.emplace_back(std::make_unique<Task>(std::move(pack), life));
		return task_list_.back().get();
	}
	// 3. 池子满了，又无空闲可复用
	return nullptr;
}


//检查任务状态成功并重置任务
//返回一个空闲任务容器的引索
size_t TasksPool::CheckTask()
{
	for (auto &task : task_list_)
	{
		if (task->CheckExecuted());
	}
   return;
}
