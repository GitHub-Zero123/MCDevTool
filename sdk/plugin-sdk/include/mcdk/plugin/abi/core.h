/*
 * MCDK 插件 ABI —— 核心类型
 *
 * 纯 C99。abi/ 下除 abi_assert.h 外的每个头文件都必须能被 C 编译器单独编译，
 * 除 <stddef.h> / <stdint.h> 外无任何依赖。
 *
 * 完整契约与每条禁令的理由见 docs/plugin-system/02-abi-contract.md。
 */
#ifndef MCDK_PLUGIN_ABI_CORE_H
#define MCDK_PLUGIN_ABI_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 版本                                                                */
/* ------------------------------------------------------------------ */
/*
 * major 不一致直接拒绝加载。plugin.minor > host.minor 同样拒绝，并提示升级
 * mcdk。新增接口表、或往已有接口表尾部追加字段，都必须递增 minor——struct_size
 * 负责运行期的字段探测，minor 负责在 LoadLibrary 之前就给出可读的拒绝理由。
 */
#define MCDK_ABI_VERSION_MAJOR 1
#define MCDK_ABI_VERSION_MINOR 0

/* ------------------------------------------------------------------ */
/* 调用约定与符号可见性                                                  */
/* ------------------------------------------------------------------ */
/* x64 只有一种调用约定，此处显式标注是为了 x86 与将来其他平台不出分歧。 */
#if defined(_WIN32) && !defined(_WIN64)
#define MCDK_CALL __cdecl
#else
#define MCDK_CALL
#endif

/* 插件只导出入口符号，其余一律 hidden。由 mcdk_add_plugin() 配合设置。 */
#if defined(_WIN32)
#define MCDK_PLUGIN_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define MCDK_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define MCDK_PLUGIN_EXPORT
#endif

/* ------------------------------------------------------------------ */
/* 基础标量                                                            */
/* ------------------------------------------------------------------ */
/*
 * 一律定宽。禁止 long / unsigned long（Windows 4 字节、LP64 8 字节）、
 * wchar_t（MSVC 16 位、GCC 32 位）、以及 C++ 的 bool（大小未由标准固定）。
 */
typedef uint8_t  mcdk_bool;   /* 取值只能是 0 或 1 */
typedef int32_t  mcdk_status; /* MCDK_OK，或负的错误码 */
typedef uint64_t mcdk_handle; /* 不透明句柄，0 表示无效 */

#define MCDK_FALSE ((mcdk_bool)0)
#define MCDK_TRUE  ((mcdk_bool)1)

/* ------------------------------------------------------------------ */
/* 字符串                                                              */
/* ------------------------------------------------------------------ */
/*
 * UTF-8 视图，不保证以 NUL 结尾。默认借用语义：指针仅在被调函数返回前有效，
 * 调用方如需保留必须立即深拷贝。
 *
 * 之所以不用 C++ 字符串：std::string 在 MSVC STL / libstdc++ / libc++ 之间
 * 布局完全不同，即便同为 MSVC STL，_ITERATOR_DEBUG_LEVEL 不同（Debug 与
 * Release 混用）布局也不同，而后者跨 DLL 边界时不会报错、只会静默损坏内存。
 * 边界上因此只有这个两字段的 POD，两侧各自在自己的 shim 里与本地字符串互转。
 *
 * 同理，边界上不出现任何 std::vector / std::function / std::filesystem::path /
 * nlohmann::json：容器走回调式枚举或 (ptr, count) 视图，可调用体走
 * 函数指针 + void* userdata，路径走 UTF-8 generic 形式的 mcdk_str，
 * JSON 走 UTF-8 文本。详见 02-abi-contract.md §3.3 的消化清单。
 */
typedef struct mcdk_str {
    const char* ptr; /* len == 0 时允许为 NULL */
    size_t      len; /* 字节数，不含结尾 NUL */
} mcdk_str;

/* ------------------------------------------------------------------ */
/* 状态码                                                              */
/* ------------------------------------------------------------------ */
/*
 * 已分配的数值永不复用、永不改变（02-abi-contract.md §5.10）。
 * 新增取值登记在 docs/plugin-system/13-registry.md §5。
 */
enum {
    MCDK_OK = 0,

    MCDK_ERR_INVALID_ARGUMENT = -1,
    MCDK_ERR_INVALID_HANDLE   = -2,
    MCDK_ERR_NOT_SUPPORTED    = -3, /* 该宿主版本没有此能力 */
    MCDK_ERR_WRONG_STAGE      = -4, /* 在错误的生命周期阶段调用 */
    MCDK_ERR_OUT_OF_MEMORY    = -5,
    MCDK_ERR_TIMEOUT          = -6,
    MCDK_ERR_GAME_NOT_READY   = -7,
    MCDK_ERR_DUPLICATE        = -8,
    MCDK_ERR_BUFFER_TOO_SMALL = -9,

    MCDK_ERR_PLUGIN_EXCEPTION = -100, /* 插件侧异常屏障捕获 */
    MCDK_ERR_HOST_EXCEPTION   = -101  /* 宿主侧异常屏障捕获 */
};

/* ------------------------------------------------------------------ */
/* 生命周期阶段                                                         */
/* ------------------------------------------------------------------ */
/*
 * 禁止 typedef enum：枚举底层类型在不同编译器/选项下可能收缩或扩展。
 * 一律定宽整数 + 匿名枚举常量。
 */
typedef uint32_t mcdk_stage;
enum {
    MCDK_STAGE_REGISTER = 0, /* 只能注册：事件、MCP 工具 */
    MCDK_STAGE_CONFIG   = 1, /* UserConfig 已解析 */
    MCDK_STAGE_WORLD    = 2, /* 世界目录与 pack manifest 已就绪 */
    MCDK_STAGE_RUNTIME  = 3, /* 运行期子系统均已就绪 */
    MCDK_STAGE_SHUTDOWN = 4
};

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MCDK_PLUGIN_ABI_CORE_H */
