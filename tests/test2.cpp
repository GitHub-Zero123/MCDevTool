#include <iostream>
#include <string>
#include <thread>
#include <functional>
#include <vector>
#include <mcdevtool.h>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int main() {
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);

    auto modDirs = std::vector<std::string_view>{
        "D:\\Zero123\\Cpp\\__TEST__\\MCDevTool\\tests\\reloadFiles\\"
    };

    auto thread = MCDevTool::HotReload::watchAndReloadPyFiles(
        modDirs,
        [](const std::filesystem::path& changedFile) {
            std::cout << "Detected change in: " << changedFile.generic_string() << std::endl;
        }
    );
    if(thread.has_value()) {
        std::cout << "Started watching for file changes..." << std::endl;
        thread->join();
    } else {
        std::cerr << "Failed to start file watcher." << std::endl;
        return 1;
    }
    return 0;
}