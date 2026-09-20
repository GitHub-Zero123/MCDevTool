/*
 * MCDK 插件 ABI —— mcdk.events/1
 *
 * 纯 C99。约束见 ../core.h 顶部说明。
 */
#ifndef MCDK_PLUGIN_ABI_IFACE_EVENTS_H
#define MCDK_PLUGIN_ABI_IFACE_EVENTS_H

#include "../core.h"
#include "../events.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MCDK_IFACE_EVENTS_NAME "mcdk.events"
#define MCDK_IFACE_EVENTS_VERSION 1u

typedef struct mcdk_iface_events {
    uint32_t struct_size;
    uint32_t _reserved;

    /*
     * 事件名 → 本次运行内的数值 id。未知名字返回 0。
     * 插件应据此优雅降级（新宿主上多用一个事件、旧宿主上少用一个），
     * 而不是判定加载失败。
     */
    uint32_t(MCDK_CALL* resolve)(mcdk_handle self, mcdk_str name);

    /*
     * 返回退订用的 token；失败返回 0（未知 event_id、handler 为空、阶段不对）。
     *
     * priority 小者先执行；相同 priority 按插件在 .mcdev.json 中的声明顺序。
     *
     * 事件不重放：在某事件已经发射之后才订阅，不会补发。依赖早期事件的订阅
     * 必须在 MCDK_STAGE_REGISTER 内完成。
     */
    mcdk_handle(MCDK_CALL* subscribe)(
        mcdk_handle        self,
        uint32_t           event_id,
        mcdk_dispatch_mode mode,
        int32_t            priority,
        mcdk_event_handler handler,
        void*              user
    );

    void(MCDK_CALL* unsubscribe)(mcdk_handle self, mcdk_handle token);

    /* 插件发射自己的事件。名字必须用自己的反向域名前缀，禁止占用 mcdk. 命名空间。 */
    mcdk_status(MCDK_CALL* emit)(mcdk_handle self, uint32_t event_id, const void* payload, uint32_t payload_size);

    /*
     * 把一段工作投递到主线程。主线程在阶段推进点与游戏等待循环中抽取，
     * 因此延迟是几十毫秒量级，不适合做需要即时响应的事。
     */
    void(MCDK_CALL* post_main)(mcdk_handle self, void(MCDK_CALL* fn)(void*), void* user);
} mcdk_iface_events;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MCDK_PLUGIN_ABI_IFACE_EVENTS_H */
