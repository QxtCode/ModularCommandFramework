/// Test DLL — 导出 TestDLLModule + 计数器访问器
#include "TestDLLModule.h"
#include "core/ModuleLifeManager.h"

// ---- 全局计数器 ----
std::atomic<int> g_test_dll_init_count{0};
std::atomic<int> g_test_dll_shutdown_count{0};
std::atomic<int> g_test_dll_exec_count{0};
bool g_test_dll_oninit_result = true;

// ---- DLL 导出接口 ----
EXPORT_MODULE(TestDLLModule)

// ---- 计数器访问器（测试通过 GetProcAddress 调用）----
extern "C" __declspec(dllexport) int  GetTestDLLInitCount()    { return g_test_dll_init_count.load(); }
extern "C" __declspec(dllexport) int  GetTestDLLShutdownCount(){ return g_test_dll_shutdown_count.load(); }
extern "C" __declspec(dllexport) int  GetTestDLLExecCount()    { return g_test_dll_exec_count.load(); }
extern "C" __declspec(dllexport) void SetTestDLLOnInitResult(bool ok) { g_test_dll_oninit_result = ok; }
extern "C" __declspec(dllexport) void ResetTestDLLCounters()
{
    g_test_dll_init_count.store(0);
    g_test_dll_shutdown_count.store(0);
    g_test_dll_exec_count.store(0);
    g_test_dll_oninit_result = true;
}
