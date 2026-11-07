#pragma once
// 插件截图的宿主侧持有表。
// 图像使用宿主持有的句柄和调用方缓冲，避免跨模块共享分配器。
#include <cstdint>
#include <memory>
#include <vector>

#include <mcdk/plugin/abi/core.h>

namespace mcdk::plugin_host::detail {

    struct ImageRecord {
        std::vector<std::uint8_t> bytes;
        std::uint32_t             width  = 0;
        std::uint32_t             height = 0;
    };

    // 返回新句柄；owner 是插件句柄。
    [[nodiscard]] mcdk_handle addImage(mcdk_handle owner, ImageRecord record);
    // 只有 owner 本人能取到自己的图像：句柄猜测不应该能跨插件读到数据。
    // 返回 shared_ptr 而非裸指针，是因为调用方必然要在锁外使用它：插件完全可以
    [[nodiscard]] std::shared_ptr<const ImageRecord> findImage(mcdk_handle owner, mcdk_handle image) noexcept;

    void releaseImage(mcdk_handle owner, mcdk_handle image) noexcept;

    // 插件终结时调用，返回被回收的遗留图像数（即泄漏数）。
    std::size_t releaseAllImages(mcdk_handle owner) noexcept;

} // namespace mcdk::plugin_host::detail
