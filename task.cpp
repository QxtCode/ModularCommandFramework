#include"Task.h"
#include"Parmer_Packe.h"
#include"Module_Life_Manager.h"
#include<chrono>

Task::Task(std::unique_ptr<ParmarPack> pack, ModuleLifeManager& lifemaager)
	:m_pack_(std::move (pack)),
	m_lifemaager_(lifemaager)
{}

bool Task::run()
{

	if (m_pack_)
	{
		if(!m_lifemaager_.Dispatch(m_pack_.get())) return false;
		this->CheckExecuted();
	}
	return true;
}