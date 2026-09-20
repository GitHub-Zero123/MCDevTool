/*
 * MCDK 插件 ABI —— mcdk.game/1
 *
 * 纯 C99。约束见 ../core.h 顶部说明。
 */
#ifndef MCDK_PLUGIN_ABI_IFACE_GAME_H
#define MCDK_PLUGIN_ABI_IFACE_GAME_H

#include "../core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MCDK_IFACE_GAME_NAME    "mcdk.game"
#define MCDK_IFACE_GAME_VERSION 1u

typedef uint32_t mcdk_side;
enum {
    MCDK_SIDE_SERVER = 0,
    MCDK_SIDE_CLIENT = 1
};

typedef uint32_t mcdk_image_format;
enum {
    MCDK_IMAGE_JPEG = 0
};

typedef struct mcdk_capture_options {
    uint32_t struct_size;
    /* 等比缩放高度上限，不放大小窗口；0 = 宿主默认（480）。 */
    uint32_t max_height;
    /*
     * 客户区内的截取范围，归一化到 0.0~1.0：(0,0) 左上、(1,1) 右下，与 mc_input
     * 的坐标系同构。四个值全为 0 表示整块客户区。
     *
     * 用归一化而非像素，是因为插件拿不到客户区尺寸——它既不持有窗口句柄，也不该
     * 持有。归一化让「截右下角那块」这种意图在任何分辨率下都成立。
     */
    double region_left;
    double region_top;
    double region_right;
    double region_bottom;
} mcdk_capture_options;

typedef struct mcdk_image_info {
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    uint32_t format; /* mcdk_image_format */
    size_t   byte_size;
} mcdk_image_info;

typedef struct mcdk_iface_game {
    uint32_t struct_size;
    uint32_t _reserved;

    /*
     * 在游戏进程里执行 Python 并取回返回值，阻塞至游戏返回或超时。
     * out_result_json 借用，指向宿主线程局部缓冲，必须立即拷贝。
     *
     * timeout_ms 被钳制到 [1, 120000]；传 0 表示用宿主默认（10000）。
     * 游戏未进入世界或调试 IPC 无客户端时返回 MCDK_ERR_GAME_NOT_READY。
     *
     * 禁止在 MCDK_DISPATCH_SYNC 派发的事件处理器中调用——尤其是 mcdk.log.line：
     * 那个回调跑在日志读取线程上，阻塞它会卡住整条游戏日志管道，而 Python 执行
     * 本身又会产生日志，构成自锁。改用 QUEUED 或经 post_main 转手。
     * 在 MCP 工具 handler 中调用是安全的，那是它最主要的用法。
     */
    mcdk_status(MCDK_CALL* execute_python)(
        mcdk_handle self,
        mcdk_str    code,
        mcdk_side   side,
        uint32_t    timeout_ms,
        mcdk_str*   out_result_json
    );

    /*
     * 截取游戏窗口，输出 JPEG。宿主持有图像，插件拷走后必须 release。
     *
     * 采用宿主持有句柄而非共享分配器，是为了让边界上不出现任何分配器穿越
     * （02-abi-contract.md §6）。非 Windows 平台返回 MCDK_ERR_NOT_SUPPORTED。
     */
    mcdk_status(MCDK_CALL* capture_window)(
        mcdk_handle                 self,
        const mcdk_capture_options* options,
        mcdk_handle*                out_image
    );

    mcdk_status(MCDK_CALL* image_get_info)(mcdk_handle self, mcdk_handle image, mcdk_image_info* out_info);

    /*
     * 拷入调用方缓冲。capacity 小于 byte_size 时不写入任何数据，
     * 回填所需长度到 out_written 并返回 MCDK_ERR_BUFFER_TOO_SMALL。
     */
    mcdk_status(MCDK_CALL* image_copy)(
        mcdk_handle self,
        mcdk_handle image,
        void*       buffer,
        size_t      capacity,
        size_t*     out_written
    );

    /*
     * 释放图像句柄。必须被调用。
     * 宿主会在 MCDK_STAGE_SHUTDOWN 清理该插件遗留的全部图像并就每个泄漏项告警——
     * 但那是兜底，不是许可。
     */
    void(MCDK_CALL* image_release)(mcdk_handle self, mcdk_handle image);
} mcdk_iface_game;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MCDK_PLUGIN_ABI_IFACE_GAME_H */
