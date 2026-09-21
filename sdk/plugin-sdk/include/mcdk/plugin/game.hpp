#pragma once
// mcdk.game 的 C++ 封装。
// 截图在 ABI 上是「宿主持句柄 + 四步拷贝 + 必须 release」，很容易漏掉最后一步。
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "abi/iface/game.h"
#include "detail/abi_bridge.hpp"
#include "error.hpp"

namespace mcdk {

    enum class Side {
        Server,
        Client
    };

    // 截图参数。region 归一化到 0.0~1.0，(0,0) 左上、(1,1) 右下。
    // 四个值全为 0（默认）表示整块客户区。
    struct CaptureOptions {
        std::uint32_t maxHeight    = 0; // 0 = 宿主默认（480）
        double        regionLeft   = 0.0;
        double        regionTop    = 0.0;
        double        regionRight  = 0.0;
        double        regionBottom = 0.0;
    };

    struct CapturedImage {
        std::uint32_t             width  = 0;
        std::uint32_t             height = 0;
        std::vector<std::uint8_t> jpeg;

        [[nodiscard]] bool empty() const noexcept { return jpeg.empty(); }
    };

    class Game {
    public:
        Game() = default;

        Game(mcdk_handle self, const mcdk_iface_game* table) noexcept : mSelf(self), mTable(table) {}

        [[nodiscard]] bool available() const noexcept { return mTable != nullptr; }
        // 在游戏进程里执行 Python 并取回 JSON 文本形式的返回值。
        // 阻塞调用。禁止在 Dispatch::Sync 的事件处理器里用——尤其是 ev::LogLine，
        [[nodiscard]] std::string executePython(
            std::string_view code,
            Side             side      = Side::Server,
            std::uint32_t    timeoutMs = 0
        ) const {
            std::string result;
            const auto  status = tryExecutePython(code, result, side, timeoutMs);
            if (status != MCDK_OK) {
                throw Error(status, "mcdk.game.execute_python failed");
            }
            return result;
        }

        [[nodiscard]] mcdk_status tryExecutePython(
            std::string_view code,
            std::string&     outResultJson,
            Side             side      = Side::Server,
            std::uint32_t    timeoutMs = 0
        ) const {
            if (!detail::ifaceHas(mTable, &mcdk_iface_game::execute_python)) {
                return MCDK_ERR_NOT_SUPPORTED;
            }
            mcdk_str   raw{};
            const auto status = mTable->execute_python(
                mSelf,
                detail::toAbi(code),
                side == Side::Client ? MCDK_SIDE_CLIENT : MCDK_SIDE_SERVER,
                timeoutMs,
                &raw
            );
            if (status == MCDK_OK) {
                // 借用的，必须在这里就拷走。
                outResultJson = detail::toString(raw);
            }
            return status;
        }

        // 截取游戏窗口。返回的 jpeg 为空即表示失败——句柄的申请与释放都在这里面
        // 完成，调用方没有机会漏掉 image_release。
        [[nodiscard]] CapturedImage capture(const CaptureOptions& options = {}) const {
            CapturedImage result;
            if (!detail::ifaceHas(mTable, &mcdk_iface_game::image_release)) {
                return result;
            }

            mcdk_capture_options raw{};
            raw.struct_size   = static_cast<std::uint32_t>(sizeof(raw));
            raw.max_height    = options.maxHeight;
            raw.region_left   = options.regionLeft;
            raw.region_top    = options.regionTop;
            raw.region_right  = options.regionRight;
            raw.region_bottom = options.regionBottom;

            mcdk_handle image = 0;
            if (mTable->capture_window(mSelf, &raw, &image) != MCDK_OK || image == 0) {
                return result;
            }

            mcdk_image_info info{};
            info.struct_size = static_cast<std::uint32_t>(sizeof(info));
            if (mTable->image_get_info(mSelf, image, &info) == MCDK_OK) {
                result.width  = info.width;
                result.height = info.height;
                result.jpeg.resize(info.byte_size);
                std::size_t written = 0;
                if (mTable->image_copy(mSelf, image, result.jpeg.data(), result.jpeg.size(), &written) != MCDK_OK) {
                    result.jpeg.clear();
                } else {
                    result.jpeg.resize(written);
                }
            }
            mTable->image_release(mSelf, image);
            return result;
        }

    private:
        mcdk_handle            mSelf  = 0;
        const mcdk_iface_game* mTable = nullptr;
    };

} // namespace mcdk
