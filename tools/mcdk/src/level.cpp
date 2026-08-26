#include <mcdk/level.hpp>

#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

#include <mcdevtool/level.h>
#include <mcdevtool/utils.h>
#include <nlohmann/json.hpp>

namespace mcdk {

    MCDevTool::Level::ClientVersion readClientVersion(const std::filesystem::path& gameExecutablePath) {
        const auto    manifestPath = gameExecutablePath.parent_path() / "data/behavior_packs/vanilla/manifest.json";
        std::ifstream manifestFile(manifestPath, std::ios::binary);
        if (!manifestFile) {
            throw std::runtime_error("无法读取客户端原版行为包版本: " + MCDevTool::Utils::pathToUtf8(manifestPath));
        }

        const auto manifest = nlohmann::json::parse(manifestFile, nullptr, false, true);
        if (manifest.is_discarded()) {
            throw std::runtime_error(
                "客户端原版行为包manifest.json格式无效: " + MCDevTool::Utils::pathToUtf8(manifestPath)
            );
        }

        const auto header = manifest.find("header");
        if (header == manifest.end() || !header->is_object()) {
            throw std::runtime_error("客户端原版行为包manifest.json缺少header");
        }
        const auto version = header->find("min_engine_version");
        if (version == header->end() || !version->is_array() || version->size() != 3) {
            throw std::runtime_error("客户端原版行为包manifest.json中的min_engine_version必须包含3个整数");
        }

        MCDevTool::Level::ClientVersion result{};
        for (std::size_t index = 0; index < result.size(); ++index) {
            if (!(*version)[index].is_number_integer()) {
                throw std::runtime_error("客户端原版行为包manifest.json中的min_engine_version必须包含3个整数");
            }
            const auto value = (*version)[index].get<int64_t>();
            if (value < 0 || value > std::numeric_limits<int32_t>::max()) {
                throw std::runtime_error("客户端原版行为包manifest.json中的min_engine_version超出有效范围");
            }
            result[index] = static_cast<int32_t>(value);
        }
        return result;
    }

    std::vector<uint8_t>
    createUserLevel(const WorldProjectConfig& config, const MCDevTool::Level::ClientVersion& clientVersion) {
        auto levelDat = MCDevTool::Level::createDefaultLevelDat(config.name, config.level);
        MCDevTool::Level::updateLevelDatVersion(levelDat, clientVersion);
        return levelDat;
    }

} // namespace mcdk
