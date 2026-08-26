#include <mcdk/level.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <mcdevtool/level.h>
#include <nbt/NBT.hpp>

namespace {
    void require(bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    nbt::CompoundTag parseLevel(const std::vector<uint8_t>& bytes) {
        const auto content = std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
        auto       level   = nbt::io::parseFromContent(content);
        require(level.has_value(), "Generated level.dat could not be parsed.");
        return std::move(*level);
    }
} // namespace

int main() {
    const auto uniqueId = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root =
        std::filesystem::temp_directory_path() / ("mcdevtool-level-version-test-" + std::to_string(uniqueId));
    std::filesystem::create_directories(root / "data/behavior_packs/vanilla");

    const auto executable = root / "Minecraft.Windows.exe";
    std::ofstream(executable).put('\0');
    {
        std::ofstream manifest(root / "data/behavior_packs/vanilla/manifest.json");
        manifest << R"({"header":{"min_engine_version":[1,21,120]}})";
    }

    const auto version = mcdk::readClientVersion(executable);
    require(version == MCDevTool::Level::ClientVersion{1, 21, 120}, "Client version was parsed incorrectly.");

    mcdk::WorldProjectConfig config;
    config.name = "Version Test";
    auto level  = mcdk::createUserLevel(config, version);
    auto tag    = parseLevel(level);

    require(!tag.contains("NetworkVersion"), "Stale NetworkVersion was retained.");
    require(tag.getString("InventoryVersion") != nullptr, "InventoryVersion is missing.");
    require(tag.getString("InventoryVersion")->view() == "1.21.120", "InventoryVersion is incorrect.");

    const auto minimum = tag.getList("MinimumCompatibleClientVersion");
    require(minimum != nullptr && minimum->size() == 5, "MinimumCompatibleClientVersion is malformed.");
    const int expected[] = {1, 21, 120, 0, 0};
    for (std::size_t index = 0; index < minimum->size(); ++index) {
        require(
            minimum->at(index).getType() == nbt::Tag::Type::Int
                && minimum->at(index).as<nbt::IntTag>().storage() == expected[index],
            "MinimumCompatibleClientVersion contains an incorrect value."
        );
    }

    tag["NetworkVersion"]   = nbt::IntTag(860);
    const auto serialized   = tag.toBinaryNbtWithHeader();
    auto       currentLevel = std::vector<uint8_t>(serialized.begin(), serialized.end());
    MCDevTool::Level::updateLevelDatVersion(currentLevel, version);
    require(parseLevel(currentLevel).getInt("NetworkVersion") != nullptr, "Current NetworkVersion was removed.");

    {
        std::ofstream manifest(root / "data/behavior_packs/vanilla/manifest.json", std::ios::trunc);
        manifest << R"({"header":{"min_engine_version":[1,"21",120]}})";
    }
    bool rejectedInvalidVersion = false;
    try {
        (void)mcdk::readClientVersion(executable);
    } catch (const std::runtime_error&) {
        rejectedInvalidVersion = true;
    }
    require(rejectedInvalidVersion, "Invalid client version was accepted.");

    std::filesystem::remove_all(root);
    return 0;
}
