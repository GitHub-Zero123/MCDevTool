#include <cstdlib>
#include <iostream>
#include <string_view>

#include <nbt/NBT.hpp>
#include <nlohmann/json.hpp>

#include "../tools/mcdk/modules/level.hpp"

namespace {

    int fail(std::string_view message) {
        std::cerr << message << "\n";
        return EXIT_FAILURE;
    }

    bool getByte(const nbt::CompoundTag& tag, std::string_view key) {
        return static_cast<uint8_t>(tag.at(key).as<nbt::ByteTag>()) != 0;
    }

    int32_t getInt(const nbt::CompoundTag& tag, std::string_view key) {
        return static_cast<int32_t>(tag.at(key).as<nbt::IntTag>());
    }

    int64_t getLong(const nbt::CompoundTag& tag, std::string_view key) {
        return static_cast<int64_t>(tag.at(key).as<nbt::LongTag>());
    }

} // namespace

int main() {
    nlohmann::json config{
        {"world_name", "MCDK_RULE_TEST"},
        {"do_mob_spawning", false},
        {"do_mob_loot", false},
        {"mob_griefing", false},
        {"bonus_chest", true},
        {"set_world_time_on_start", true},
        {"world_time", 13000},
    };

    auto levelDat = mcdk::createUserLevel(config);
    auto content  = std::string_view{reinterpret_cast<const char*>(levelDat.data()), levelDat.size()};
    auto nbtData  = nbt::io::parseFromContent(content);
    if (!nbtData.has_value()) {
        return fail("无法解析生成的level.dat");
    }

    const auto& tag = nbtData.value();
    if (getByte(tag, "domobspawning")) {
        return fail("domobspawning未按配置关闭");
    }
    if (getByte(tag, "spawnMobs")) {
        return fail("spawnMobs未按配置关闭");
    }
    if (getByte(tag, "domobloot")) {
        return fail("domobloot未按配置关闭");
    }
    if (getByte(tag, "mobgriefing")) {
        return fail("mobgriefing未按配置关闭");
    }
    if (!getByte(tag, "bonusChestEnabled")) {
        return fail("bonusChestEnabled未按配置开启");
    }
    if (getLong(tag, "Time") != 13000) {
        return fail("Time未按配置写入");
    }
    if (getLong(tag, "currentTick") != 13000) {
        return fail("currentTick未按配置写入");
    }
    if (getInt(tag, "daylightCycle") != 13000) {
        return fail("daylightCycle未按配置写入");
    }

    auto nullTimeOptions = mcdk::parseLevelOptionsFromUserConfig({
        {"set_world_time_on_start", true},
        {"world_time", nullptr},
    });
    if (nullTimeOptions.setWorldTimeOnStart) {
        return fail("world_time为null时不应启用时间覆盖");
    }

    auto clampedOptions = mcdk::parseLevelOptionsFromUserConfig({
        {"set_world_time_on_start", true},
        {"world_time", 25000},
    });
    if (!clampedOptions.setWorldTimeOnStart || clampedOptions.worldTime != 24000) {
        return fail("world_time未被裁剪到0..24000");
    }

    return EXIT_SUCCESS;
}
