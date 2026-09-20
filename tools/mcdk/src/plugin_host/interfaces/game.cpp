//
// mcdk.game/1 的宿主实现。
//
// 本文件里的每个导出函数都必须经过 guard / guardVoid，没有例外
// （docs/plugin-system/02-abi-contract.md §4.3）。
//

#include <algorithm>
#include <cstring>
#include <string>

#include <nlohmann/json.hpp>

#include <mcdk/ipc_code_execution.hpp>
#include <mcdk/plugin/abi/iface/game.h>
#include <mcdk/plugin_host/guard.hpp>

#ifdef _WIN32
#include <mcdevtool/style.h>
#endif

#include "../images.hpp"
#include "../registry.hpp"

namespace mcdk::plugin_host::detail {

    namespace {

        constexpr std::uint32_t kDefaultTimeoutMs = 10000;
        constexpr std::uint32_t kMaxTimeoutMs     = 120000;

        // mcdk.game 仅 RUNTIME 可用（05-interfaces.md §9）。SHUTDOWN 也不行：
        // 游戏进程此时已经退出，Python 执行与截图都没有对象。
        [[nodiscard]] bool inGameStage() noexcept { return currentStage() == MCDK_STAGE_RUNTIME; }

        // execute_python 的结果按借用交付，所以要在宿主侧活过本次返回。
        // 与错误槽同一套约定：谁产生数据谁用自己的 TLS 暂存，对方立即拷走。
        [[nodiscard]] std::string& resultSlot() noexcept {
            thread_local std::string storage;
            return storage;
        }

        // 从 JPEG 的 SOFn 段读出尺寸。
        //
        // captureMinecraftWindowJpeg 只给字节流，不给宽高，而 image_get_info 里放一个
        // 恒为 0 的字段就是个陷阱——用的人迟早会信它。解析 SOF 段是二十行的事。
        bool readJpegSize(const std::vector<std::uint8_t>& data, std::uint32_t& width, std::uint32_t& height) {
            if (data.size() < 4 || data[0] != 0xFF || data[1] != 0xD8) {
                return false;
            }
            std::size_t offset = 2;
            while (offset + 3 < data.size()) {
                if (data[offset] != 0xFF) {
                    ++offset; // 填充字节
                    continue;
                }
                const std::uint8_t marker = data[offset + 1];
                if (marker == 0xFF) {
                    ++offset;
                    continue;
                }
                // 无长度字段的独立标记。
                if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD9)) {
                    offset += 2;
                    continue;
                }
                if (offset + 3 >= data.size()) {
                    return false;
                }
                const std::size_t length =
                    (static_cast<std::size_t>(data[offset + 2]) << 8) | static_cast<std::size_t>(data[offset + 3]);
                // SOF0..SOF15，排除 DHT(C4)、JPGA(C8)、DAC(CC)。
                const bool isStartOfFrame =
                    marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
                if (isStartOfFrame) {
                    if (offset + 9 >= data.size()) {
                        return false;
                    }
                    height = (static_cast<std::uint32_t>(data[offset + 5]) << 8) | data[offset + 6];
                    width  = (static_cast<std::uint32_t>(data[offset + 7]) << 8) | data[offset + 8];
                    return true;
                }
                if (marker == 0xDA) {
                    return false; // 进了扫描数据还没见到 SOF
                }
                offset += 2 + length;
            }
            return false;
        }

        mcdk_status MCDK_CALL gameExecutePython(
            mcdk_handle self,
            mcdk_str    code,
            mcdk_side   side,
            uint32_t    timeout_ms,
            mcdk_str*   out_result_json
        ) noexcept {
            return guard([&]() -> mcdk_status {
                if (out_result_json == nullptr) {
                    return MCDK_ERR_INVALID_ARGUMENT;
                }
                *out_result_json = mcdk_str{};
                if (registry().find(self) == nullptr) {
                    return MCDK_ERR_INVALID_HANDLE;
                }
                if (!inGameStage()) {
                    return MCDK_ERR_WRONG_STAGE;
                }
                if (code.ptr == nullptr || code.len == 0) {
                    return MCDK_ERR_INVALID_ARGUMENT;
                }

                const auto binding = sessionBinding();
                if (!binding.ipcServer || binding.ipcServer->getClientCount() == 0) {
                    return MCDK_ERR_GAME_NOT_READY;
                }

                const std::uint32_t timeout =
                    timeout_ms == 0 ? kDefaultTimeoutMs : std::clamp(timeout_ms, 1u, kMaxTimeoutMs);

                // 借用的入参在这里就拷成 std::string：底层接口要按值收，
                // 而且这次调用会阻塞数秒，绝不能继续指着调用方的栈。
                const std::string source(code.ptr, code.len);
                const auto        result = ipc_code_execution::requestCodeReturnValueJson(
                    binding.ipcServer,
                    source,
                    side == MCDK_SIDE_CLIENT,
                    timeout
                );

                resultSlot()            = result.dump();
                out_result_json->ptr    = resultSlot().data();
                out_result_json->len    = resultSlot().size();
                return MCDK_OK;
            });
        }

        mcdk_status MCDK_CALL gameCaptureWindow(
            mcdk_handle                 self,
            const mcdk_capture_options* options,
            mcdk_handle*                out_image
        ) noexcept {
            return guard([&]() -> mcdk_status {
                if (out_image == nullptr) {
                    return MCDK_ERR_INVALID_ARGUMENT;
                }
                *out_image = 0;
                if (registry().find(self) == nullptr) {
                    return MCDK_ERR_INVALID_HANDLE;
                }
                if (!inGameStage()) {
                    return MCDK_ERR_WRONG_STAGE;
                }
                if (options != nullptr && options->struct_size < sizeof(mcdk_capture_options)) {
                    return MCDK_ERR_INVALID_ARGUMENT;
                }
#ifndef _WIN32
                (void)options;
                return MCDK_ERR_NOT_SUPPORTED;
#else
                const auto binding = sessionBinding();
                const auto gamePid = binding.gamePid ? binding.gamePid->load(std::memory_order_relaxed) : 0u;
                if (gamePid == 0) {
                    return MCDK_ERR_GAME_NOT_READY;
                }

                MCDevTool::Style::CaptureOptions captureOptions;
                if (options != nullptr) {
                    if (options->max_height != 0) {
                        captureOptions.maxHeight = options->max_height;
                    }
                    // 四个值全为 0 表示整块客户区，保持默认。
                    const bool hasRegion = options->region_left != 0.0 || options->region_top != 0.0
                                        || options->region_right != 0.0 || options->region_bottom != 0.0;
                    if (hasRegion) {
                        captureOptions.region = MCDevTool::Style::CaptureRegion{
                            options->region_left,
                            options->region_top,
                            options->region_right,
                            options->region_bottom,
                        };
                    }
                }

                auto captured =
                    MCDevTool::Style::captureMinecraftWindowJpeg(static_cast<int>(gamePid), captureOptions);
                if (!captured) {
                    switch (captured.error()) {
                    case MCDevTool::Style::CaptureError::InvalidRegion:
                        return MCDK_ERR_INVALID_ARGUMENT;
                    case MCDevTool::Style::CaptureError::Timeout:
                        return MCDK_ERR_TIMEOUT;
                    case MCDevTool::Style::CaptureError::CaptureUnavailable:
                        return MCDK_ERR_NOT_SUPPORTED;
                    default:
                        // WindowNotFound / WindowMinimized / Failed 都归为「现在拿不到画面」。
                        return MCDK_ERR_GAME_NOT_READY;
                    }
                }

                ImageRecord record;
                record.bytes = std::move(*captured);
                readJpegSize(record.bytes, record.width, record.height);
                *out_image = addImage(self, std::move(record));
                return MCDK_OK;
#endif
            });
        }

        mcdk_status MCDK_CALL gameImageGetInfo(mcdk_handle self, mcdk_handle image, mcdk_image_info* out_info) noexcept {
            return guard([&]() -> mcdk_status {
                if (out_info == nullptr || out_info->struct_size < sizeof(mcdk_image_info)) {
                    return MCDK_ERR_INVALID_ARGUMENT;
                }
                if (registry().find(self) == nullptr) {
                    return MCDK_ERR_INVALID_HANDLE;
                }
                const auto* record = findImage(self, image);
                if (record == nullptr) {
                    return MCDK_ERR_INVALID_HANDLE;
                }
                mcdk_image_info info{};
                info.struct_size = static_cast<std::uint32_t>(sizeof(info));
                info.width       = record->width;
                info.height      = record->height;
                info.format      = MCDK_IMAGE_JPEG;
                info.byte_size   = record->bytes.size();
                *out_info        = info;
                return MCDK_OK;
            });
        }

        mcdk_status MCDK_CALL gameImageCopy(
            mcdk_handle self,
            mcdk_handle image,
            void*       buffer,
            size_t      capacity,
            size_t*     out_written
        ) noexcept {
            return guard([&]() -> mcdk_status {
                if (out_written == nullptr) {
                    return MCDK_ERR_INVALID_ARGUMENT;
                }
                *out_written = 0;
                if (registry().find(self) == nullptr) {
                    return MCDK_ERR_INVALID_HANDLE;
                }
                const auto* record = findImage(self, image);
                if (record == nullptr) {
                    return MCDK_ERR_INVALID_HANDLE;
                }
                *out_written = record->bytes.size();
                if (buffer == nullptr || capacity < record->bytes.size()) {
                    // 一个字节都不写，只回填所需长度——半写的缓冲比不写更难排查。
                    return MCDK_ERR_BUFFER_TOO_SMALL;
                }
                std::memcpy(buffer, record->bytes.data(), record->bytes.size());
                return MCDK_OK;
            });
        }

        void MCDK_CALL gameImageRelease(mcdk_handle self, mcdk_handle image) noexcept {
            guardVoid([&] {
                if (registry().find(self) == nullptr) {
                    return;
                }
                releaseImage(self, image);
            });
        }

        constexpr mcdk_iface_game kTable = {
            /* struct_size    */ static_cast<uint32_t>(sizeof(mcdk_iface_game)),
            /* _reserved      */ 0u,
            /* execute_python */ &gameExecutePython,
            /* capture_window */ &gameCaptureWindow,
            /* image_get_info */ &gameImageGetInfo,
            /* image_copy     */ &gameImageCopy,
            /* image_release  */ &gameImageRelease,
        };

    } // namespace

    const mcdk_iface_game* gameTable() noexcept { return &kTable; }

} // namespace mcdk::plugin_host::detail
