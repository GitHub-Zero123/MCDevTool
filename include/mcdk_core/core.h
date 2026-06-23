// mcdk_core: 公共伞头
//
// 业务侧只需 #include <mcdk_core/core.h> 即可获得启动器 API 与底层能力。
// 该头聚合：
//   - 启动器 API（launcher.h）
//   - 配置/日志/控制台类型（modules/console.hpp）
//
// 说明：mcdk_core 默认链接 mcdevtool + mcp + MCDEV_MOD_RESOURCE（可通过 CMake option 关闭后两者）。
//       因此该伞头同时带入 mcdevtool 的核心 API（MCDevTool::* 命名空间）。
//
// By Zero123
#pragma once

#include "launcher.h"

// 同时暴露底层 mcdevtool 伞头（MCDevTool::Utils / Level / Addon / Env / Debug / Style 等）。
#include <mcdevtool.h>

// 暴露控制台类型（ConsoleColor / ConsoleOutputCallback），便于业务自定义日志回调。
#include "console.hpp"
