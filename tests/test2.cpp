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

    auto modDirs = std::vector<std::string_view>{"D:\\Zero123\\Cpp\\__TEST__\\MCDevTool\\tests\\reloadFiles\\"};

    auto thread = MCDevTool::HotReload::watchAndReloadPyFiles(modDirs, [](const std::filesystem::path& changedFile) {
        std::cout << "Detected change in: " << changedFile.generic_string() << std::endl;
    });

    if (thread.has_value()) {
        std::cout << "Started watching for file changes..." << std::endl;
    } else {
        std::cerr << "Failed to start file watcher." << std::endl;
    }

    MCDevTool::Debug::DebugIPCServer server;
    server.start();
    std::cout << "Debug IPC Server started on port: " << server.getPort() << std::endl;

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        server.sendMessage(1, std::vector<uint8_t>{'H', 'e', 'l', 'l', 'o'});
    }

    // auto thread2 = MCDevTool::HotReload::watchProcessForegroundWindow(25940, [](bool isForeground) {
    //     std::cout << "Process is " << (isForeground ? "foreground" : "background") << std::endl;
    // });

    if (thread.has_value()) {
        thread->join();
    }
    server.join();

    return 0;
}