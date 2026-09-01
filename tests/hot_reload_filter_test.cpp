#include <mcdk/hotreload.hpp>
#include <mcdk/mod_dir_config.hpp>
#include <mcdk/world_project.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

class TestPyReloadWatcherTask : public mcdk::PyReloadWatcherTask {
public:
    using mcdk::PyReloadWatcherTask::shouldWatchFile;
};

class TempDirectory {
public:
    TempDirectory() {
        const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        path              = fs::temp_directory_path() / ("mcdevtool-hot-reload-filter-" + suffix);
        fs::create_directories(path);
    }

    ~TempDirectory() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    fs::path path;
};

static void touch(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream(path).put('\n');
}

static bool expect(bool condition, const char* description) {
    if (!condition) {
        std::cerr << "Failed: " << description << '\n';
    }
    return condition;
}

static MCDevTool::Addon::PackInfo resourcePack(const fs::path& path) {
    MCDevTool::Addon::PackInfo pack;
    pack.type    = MCDevTool::Addon::PackType::RESOURCE;
    pack.srcPath = path;
    return pack;
}

static bool containsPath(const std::vector<fs::path>& paths, const fs::path& expected) {
    return std::ranges::find(paths, fs::absolute(expected).lexically_normal()) != paths.end();
}

int main() {
    TempDirectory temp;
    const auto    addonRoot = temp.path / "Addon";

    const auto firstModRoot  = addonRoot / "B" / "FirstMod";
    const auto secondModRoot = addonRoot / "B" / "SecondMod";
    touch(firstModRoot / "modMain.py");
    touch(secondModRoot / "modMain.py");

    const auto enabledResourceRoot  = temp.path / "EnabledAddon" / "R";
    const auto disabledResourceRoot = temp.path / "DisabledAddon" / "R";
    for (const auto* subdir : {"ui", "shaders", "materials", "particles"}) {
        fs::create_directories(enabledResourceRoot / subdir);
        fs::create_directories(disabledResourceRoot / subdir);
    }

    const std::vector<mcdk::UserModDirConfig> modConfigs{
        mcdk::UserModDirConfig(enabledResourceRoot.parent_path(), true, true),
        mcdk::UserModDirConfig(disabledResourceRoot.parent_path(), false, true),
    };
    const std::vector<MCDevTool::Addon::PackInfo> resourcePacks{
        resourcePack(enabledResourceRoot),
        resourcePack(disabledResourceRoot),
    };
    const auto hotReloadResourcePacks =
        mcdk::UserModDirConfig::collectHotReloadResourcePackPaths(modConfigs, resourcePacks);

    TestPyReloadWatcherTask task;
    task.setModDirs(std::vector<fs::path>{addonRoot});

    bool passed =
        expect(task.shouldWatchFile(firstModRoot / "client" / "system.py"), "Python file in first Mod package")
        && expect(task.shouldWatchFile(secondModRoot / "server.py"), "Python file in second Mod package")
        && expect(task.shouldWatchFile(firstModRoot / "modMain.py"), "modMain.py itself")
        && expect(
            !task.shouldWatchFile(addonRoot / "tools" / "test_management.py"),
            "Python test tool outside Mod packages"
        )
        && expect(
            !task.shouldWatchFile(addonRoot / "B" / "NoModMain" / "helper.py"),
            "Python directory without modMain.py"
        )
        && expect(!task.shouldWatchFile(firstModRoot / "client" / "config.json"), "Non-Python Mod package file");

    passed = expect(hotReloadResourcePacks.size() == 1, "Only hot-reload-enabled resource pack is collected") && passed;
    passed = expect(containsPath(hotReloadResourcePacks, enabledResourceRoot), "Enabled resource pack is collected")
          && passed;
    passed = expect(!containsPath(hotReloadResourcePacks, disabledResourceRoot), "Disabled resource pack is excluded")
          && passed;

    for (const auto* subdir : {"ui", "shaders", "materials", "particles"}) {
        const auto paths = mcdk::UserModDirConfig::collectResourceSubdirPaths(hotReloadResourcePacks, subdir);
        passed = expect(paths.size() == 1, "Only hot-reload-enabled resource directory is collected") && passed;
        passed = expect(containsPath(paths, enabledResourceRoot / subdir), "Enabled resource directory is collected")
              && passed;
        passed = expect(!containsPath(paths, disabledResourceRoot / subdir), "Disabled resource directory is excluded")
              && passed;
    }

    auto worldConfigs =
        std::vector<mcdk::UserModDirConfig>{mcdk::UserModDirConfig(temp.path, false, true)};
    mcdk::appendWorldHotReloadModDir(worldConfigs, temp.path);
    passed = expect(
                 worldConfigs.size() == 1 && !worldConfigs.front().hotReload,
                 "Explicit world-source hot_reload=false is preserved"
             )
          && passed;

    return passed ? 0 : 1;
}
