#include <iostream>
#include <string>
#include <filesystem>
#include <fstream>
#include <mcdevtool/env.h>
// #define CLI11_HAS_RTTI 0
// #define CLI11_HAS_FILESYSTEM 0
#include <mcdevtool/addon.h>
#include <mcdevtool/utils.h>
#include "libs/CLI11.hpp"

static void ENV_INFO() {
    using MCDevTool::Utils::pathToGenericUtf8;
    std::cout << "MCStudioDownload: " << pathToGenericUtf8(MCDevTool::autoSearchMCStudioDownloadGamePath().value_or(""))
              << "\n";
    std::cout << "Minecraft.Windows.exe: " << pathToGenericUtf8(MCDevTool::autoMatchLatestGameExePath().value_or(""))
              << "\n";
    std::cout << "MinecraftPE_Netease: " << pathToGenericUtf8(MCDevTool::getMinecraftDataPath()) << "\n";
    std::cout << "games/com.netease: " << pathToGenericUtf8(MCDevTool::getGamesComNeteasePath()) << "\n";
    std::cout << "behavior_packs: " << pathToGenericUtf8(MCDevTool::getBehaviorPacksPath()) << "\n";
    std::cout << "resource_packs: " << pathToGenericUtf8(MCDevTool::getResourcePacksPath()) << "\n";
    std::cout << "dependencies_packs: " << pathToGenericUtf8(MCDevTool::getDependenciesPacksPath()) << "\n";
    std::cout << "minecraftWorlds: " << pathToGenericUtf8(MCDevTool::getMinecraftWorldsPath()) << "\n";
}

static void CREATE_EMPTY_ADDON_PROJECT(const std::string& name) {
    namespace Addon                           = MCDevTool::Addon;
    namespace Utils                           = MCDevTool::Utils;
    auto [behaviorManifest, resourceManifest] = Addon::createEmptyAddonManifest(name, {1, 0, 0});
    auto workPath                             = std::filesystem::current_path();
    // 生成随机文件夹名称
    std::string folderName = name;
    if (folderName.empty()) {
        folderName    = Utils::createCompactUUID();
        folderName[0] = 'A';
    }
    // 生成行为包和资源包
    auto behaviorPackPath = workPath / (folderName + "B");
    auto resourcePackPath = workPath / (folderName + "R");
    std::filesystem::create_directories(behaviorPackPath);
    std::filesystem::create_directories(resourcePackPath);
    // 写入manifest文件
    {
        std::ofstream behaviorManifestFile(behaviorPackPath / "manifest.json", std::ios::binary);
        behaviorManifestFile << behaviorManifest;
        std::filesystem::create_directories(behaviorPackPath / "entities");
    }
    {
        std::ofstream resourceManifestFile(resourcePackPath / "manifest.json", std::ios::binary);
        resourceManifestFile << resourceManifest;
        std::filesystem::create_directories(resourcePackPath / "textures");
    }
    std::cout << "创建成功:\n";
    std::cout << "行为包路径: " << MCDevTool::Utils::pathToGenericUtf8(behaviorPackPath) << "\n";
    std::cout << "资源包路径: " << MCDevTool::Utils::pathToGenericUtf8(resourcePackPath) << "\n";
}

#ifdef _WIN32
int MCDK_CLI_PARSE(int argc, wchar_t* argv[]) {
#else
int MCDK_CLI_PARSE(int argc, char* argv[]) {
#endif
    CLI::App app{"MCDK CLI"};
    app.require_subcommand(1);

    // 列出环境信息
    app.add_subcommand("envinfo", "列出当前环境信息")->callback([]() { ENV_INFO(); });

    // 创建一个空的Addon项目
    auto*       create = app.add_subcommand("create", "创建一个空的Addon项目");
    std::string name;
    create->add_option("-n,--name", name, "项目名称")->default_val("auto");

    create->callback([&name]() {
        if (name == "auto") {
            name.clear();
        }
        CREATE_EMPTY_ADDON_PROJECT(name);
    });

    CLI11_PARSE(app, argc, argv);
    return 0;
}