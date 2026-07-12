#pragma once
#include<atomic>
#include<memory>
#include"Parmer_Packe.h"
class ModuleLifeManager;
class Task
{
public:
	Task(std::unique_ptr<ParmarPack> pack, ModuleLifeManager& lifemaager);
	~Task() = default;
	inline bool CheckExecuted() {
		// 1. 准备一个空壳
		std::unique_ptr<ParmarPack> repack;

		// 2. 和自己持有的指针做交换
		repack.swap(m_pack_);

		// 3. 检查空壳里有没有东西
		if (repack) {
			// repack 不为空 → 说明 m_pack_ 原来持有参数包 → 转移失败，任务失败
			is_execute_.store(0);
			// 把参数包放回去，所有权还在任务这里
			m_pack_.swap(repack);
		}
		else {
			// repack 为空 → 说明 m_pack_ 原来就是空的 → 参数包已被成功转移
			is_execute_.store(1);
			pack_null_.store(true);
		}
	}

	//设置参数包（复用）
	inline const ParmarPack* SetPack(std::unique_ptr<ParmarPack> pack)
	{
		m_pack_.swap(pack);
		return m_pack_.get();
	}
	
	////获取当前时间
	//inline long long GetCurrentTimeMs() {
	//	auto now = std::chrono::steady_clock::now();               // 1. 获取当前时间点
	//	auto duration = now.time_since_epoch();                    // 2. 从时钟纪元到现在的时长
	//	auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(duration); // 3. 转换成毫秒
	//	return millis.count();                                     // 4. 取出整数
	//}
	 // 禁用拷贝
	Task(const Task&) = delete;
	Task& operator=(const Task&) = delete;

	// 启用移动
	Task(Task&&) = default;
	Task& operator=(Task&&) = default;
	bool run();
private:
	std::atomic<bool>pack_null_{ false };//参数包否为空默认flase
	std::atomic<int>is_execute_{-1};//默认没有-1
	std::atomic<long long>start_{0};
	std::atomic<long long>end_{0};
	std::unique_ptr<ParmarPack>m_pack_;
	ModuleLifeManager& m_lifemaager_;
};