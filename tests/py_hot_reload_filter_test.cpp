#include "../tools/mcdk/modules/hotreload.hpp"

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
        path = fs::temp_directory_path() / ("mcdevtool-py-reload-filter-" + suffix);
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

int main() {
    TempDirectory temp;
    const auto    addonRoot = temp.path / "Addon";

    const auto firstModRoot  = addonRoot / "B" / "FirstMod";
    const auto secondModRoot = addonRoot / "B" / "SecondMod";
    touch(firstModRoot / "modMain.py");
    touch(secondModRoot / "modMain.py");

    TestPyReloadWatcherTask task;
    task.setModDirs(std::vector<fs::path>{addonRoot});

    const bool passed =
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

    return passed ? 0 : 1;
}
