/*
 * MCDK 插件 ABI —— mcdk.mcp/1
 *
 * 纯 C99。约束见 ../core.h 顶部说明。
 *
 * MCP 工具的描述是这套 ABI 上最复杂的一个数据结构：mcp::tool 含两棵 JSON 树和
 * 五个 std::optional。它是复杂类型降级规则的样板，见 05-interfaces.md §8.1。
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
 *
 * 运行在 MCP 工作线程上，不是主线程，且可能被并发调用——现有 RpcMethodOptions
 * 的默认 maxConcurrency 是 8，handler 必须自行保证线程安全。允许阻塞，这正是
 * mcdk.game.execute_python 的主要调用场景。
 *
 * out_result_json 指向插件侧线程局部缓冲，宿主在 handler 返回后立即拷贝。
 * 这与错误槽是同一套机制，只是方向相反：谁产生数据谁用自己的 TLS 暂存，
 * 对方立即拷走，两边都不分配跨界内存（05-interfaces.md §8.4）。
 *
 * 返回非 MCDK_OK 时，宿主把 out_result_json 当作错误文本转成 MCP 错误响应。
 * （不是“从插件错误槽取”：那个槽在插件进程的 SDK 侧 TLS 里，宿主根本访问不到。
 * SDK 的 handler 蹦床会在失败时把异常信息写进这个同一个 TLS 缓冲。）
 * out_result_json 为空时宿主退而报出状态码。
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
     * 注册一个 MCP 工具。只能在 MCDK_STAGE_REGISTER 调用，通常写在
     * mcdk.mcp.register.before 事件处理器里；注册表封存后返回 MCDK_ERR_WRONG_STAGE。
     *
     * 工具名冲突返回 MCDK_ERR_DUPLICATE，禁止后注册者覆盖先注册者——否则插件的
     * 加载顺序会悄悄改变 AI 看到的工具语义，这类问题排查时几乎无迹可循。
     * 插件应该给自己的工具名加可辨识的前缀。
     *
     * input_schema_json 必须是合法的 JSON 对象文本，否则返回
     * MCDK_ERR_INVALID_ARGUMENT 并在错误槽给出解析位置。
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
