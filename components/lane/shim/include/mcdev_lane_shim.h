#pragma once

// mcdk 与 mcdev-lane-shim.dll 之间的约定。双方各自编译，只靠这些常量对齐。
// 机制说明见 docs/lane.md。

// Lane 目录：游戏取 Roaming AppData 时得到的路径。由 mcdk 写进游戏进程的环境。
#define MCDEV_LANE_ROOT_ENV L"MCDEV_LANE_ROOT"

// shim 完成布防后置位的事件，名字里带游戏 pid。mcdk 恢复主线程后等它。
#define MCDEV_LANE_ARMED_EVENT_FORMAT L"Local\\mcdev-lane-armed-%lu"

// shim 无法保证重定向时直接结束游戏，用这两个退出码告诉 mcdk 原因。
#define MCDEV_LANE_EXIT_SETUP_FAILED 0x4D434C31u // 环境缺失或 MinHook 初始化失败
#define MCDEV_LANE_EXIT_HOOK_FAILED  0x4D434C32u // windows.storage 载入后装钩子失败
