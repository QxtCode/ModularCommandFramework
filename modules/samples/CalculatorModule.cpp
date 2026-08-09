/// CalculatorModule DLL 入口
/// 包含模块头 + EXPORT_MODULE 宏 → 框架通过 LoadLibrary 加载

#include "CalculatorModule.h"
#include "core/ModuleLifeManager.h"  // EXPORT_MODULE 宏

// 这一行就是 DLL 导出接口，框架用 CreateModule() / DestroyModule() 调用
EXPORT_MODULE(CalculatorModule)
