/// CalculatorModule DLL 入口 —— 纯 SDK 模块（不链 eventbus）
/// 一行 EXPORT_MODULE 生成 CreateModule / DestroyModule 导出，框架据此加载。

#include "CalculatorModule.h"   // 内含 sdk/sdk.h（含 EXPORT_MODULE）

EXPORT_MODULE(CalculatorModule)
