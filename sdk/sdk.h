/// =================================================================
///  sdk.h — test_shell SDK 伞头（一个 include 到位）
/// =================================================================
///
///  写一个模块只需要：
///     #include "sdk/sdk.h"
///     class MyModule : public ModuleBase { ... REGISTER_FUNC ... };
///
///  本伞头只拉入「纯契约 + 纯基类」，不依赖 EventBus、不依赖 core、
///  不需要链接 eventbus 动态库。接线由宿主侧完成。
/// =================================================================

#pragma once

#include "ParmarPack.h"
#include "IModule.h"
#include "ModuleBase.h"
#include "Export.h"
