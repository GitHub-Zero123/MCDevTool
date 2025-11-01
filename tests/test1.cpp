#include <iostream>
#include <string>
#include <filesystem>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int main() {
    SetConsoleOutputCP(CP_UTF8);
    const char* appData = std::getenv("APPDATA");
    if (!appData) {
        std::cerr << "APPDATA not found!\n";
        return 1;
    }
    auto path = std::filesystem::path(appData) / "MinecraftPE_Netease";
    auto u8Path = path.generic_u8string();
    std::cout << "MinecraftPE_Netease path: " << reinterpret_cast<const char*>(u8Path.c_str()) << "\n\n";

    auto gamePath = path / "games/com.netease";
    auto u8GamePath = gamePath.generic_u8string();
    std::cout << "Game path: " << reinterpret_cast<const char*>(u8GamePath.c_str()) << "\n\n";

    auto worldPath = path / "minecraftWorlds";
    auto u8WorldPath = worldPath.generic_u8string();
    std::cout << "Worlds path: " << reinterpret_cast<const char*>(u8WorldPath.c_str()) << "\n\n";
    return 0;
}