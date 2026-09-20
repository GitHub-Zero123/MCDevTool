/*
 * MCDK 插件 ABI —— mcdk.mcp/1
 * 纯 C99。约束见 ../core.h 顶部说明。
 */
#ifndef MCDK_PLUGIN_ABI_IFACE_MCP_H
#define MCDK_PLUGIN_ABI_IFACE_MCP_H

#include "../core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MCDK_IFACE_MCP_NAME    "mcdk.mcp"
#define MCDK_IFACE_MCP_VERSION 1u

/*
 * 注解位掩码。禁止用位域（见 02 §5.4：位域的布局由编译器决定），
 * 用显式掩码常量。
 */
typedef uint32_t mcdk_mcp_annotation;
enum {
    MCDK_MCP_ANNOTATION_READ_ONLY   = 1u << 0,
    MCDK_MCP_ANNOTATION_DESTRUCTIVE = 1u << 1,
    MCDK_MCP_ANNOTATION_IDEMPOTENT  = 1u << 2,
    MCDK_MCP_ANNOTATION_OPEN_WORLD  = 1u << 3
};

typedef struct mcdk_mcp_tool_desc {
    uint32_t struct_size;
    /* 哪些注解被显式设置（对应 C++ 侧 std::optional 的 has_value）。 */
    uint32_t annotation_present;
    /* 被设置的那些注解各自的值；未在 present 中置位的位无意义。 */
    uint32_t annotation_value;
    uint32_t _reserved;
    mcdk_str name;
    mcdk_str description;
    mcdk_str title;              /* annotations.title；len == 0 表示未设置 */
    mcdk_str input_schema_json;  /* JSON 文本，必填 */
    mcdk_str output_schema_json; /* JSON 文本；len == 0 表示未设置 */
} mcdk_mcp_tool_desc;

/*
 * 工具处理器。
 * 运行在 MCP 工作线程上，不是主线程，且可能被并发调用——现有 RpcMethodOptions
 */
typedef mcdk_status(MCDK_CALL* mcdk_mcp_tool_handler)(
    void*     user,
    mcdk_str  arguments_json, /* 借用 */
    mcdk_str  session_id,     /* 借用，MCP 会话标识 */
    mcdk_str* out_result_json
);

typedef struct mcdk_iface_mcp {
    uint32_t struct_size;
    uint32_t _reserved;

    /*
     * 注册 MCP 工具。仅 MCDK_STAGE_REGISTER 可调用，注册表封存后返回 MCDK_ERR_WRONG_STAGE。
    */
    mcdk_status(MCDK_CALL* add_tool)(
        mcdk_handle               self,
        const mcdk_mcp_tool_desc* desc,
        mcdk_mcp_tool_handler     handler,
        void*                     user
    );

    /* 当前已注册的全部工具，JSON 数组文本。借用，必须立即拷贝。 */
    mcdk_status(MCDK_CALL* list_tools)(mcdk_handle self, mcdk_str* out_json);
} mcdk_iface_mcp;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MCDK_PLUGIN_ABI_IFACE_MCP_H */
