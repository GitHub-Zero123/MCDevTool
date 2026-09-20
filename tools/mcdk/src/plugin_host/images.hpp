#pragma once

//
// 插件截图的宿主侧持有表。
//
// 图像走「宿主持有句柄 + 调用方缓冲」而不是共享分配器：插件与宿主可能用不同的
// CRT，一边 malloc 另一边 free 是最经典的静默堆损坏（02-abi-contract.md §6）。
// 代价是插件必须显式 release，所以这里按 owner 记账，插件终结时兜底清理并告警。
//

#include <cstdint>
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
    // 返回的指针在该图像被 release 之前有效。
    [[nodiscard]] const ImageRecord* findImage(mcdk_handle owner, mcdk_handle image) noexcept;

    void releaseImage(mcdk_handle owner, mcdk_handle image) noexcept;

    // 插件终结时调用，返回被回收的遗留图像数（即泄漏数）。
    std::size_t releaseAllImages(mcdk_handle owner) noexcept;

} // namespace mcdk::plugin_host::detail
