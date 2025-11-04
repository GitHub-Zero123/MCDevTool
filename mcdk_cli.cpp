#include <iostream>
#include <mcdevtool/env.h>
#include "libs/CLI11.hpp"

static void ENV_INFO() {
    std::cout << "MinecraftPE_Netease: " << MCDevTool::getMinecraftDataPath().generic_string() << "\n";
    std::cout << "games/com.netease: " << MCDevTool::getGamesComNeteasePath().generic_string() << "\n";
    std::cout << "behavior_packs: " << MCDevTool::getBehaviorPacksPath().generic_string() << "\n";
    std::cout << "resource_packs: " << MCDevTool::getResourcePacksPath().generic_string() << "\n";
    std::cout << "dependencies_packs: " << MCDevTool::getDependenciesPacksPath().generic_string() << "\n";
    std::cout << "minecraftWorlds: " << MCDevTool::getMinecraftWorldsPath().generic_string() << "\n";
}

int MCDK_CLI_PARSE(int argc, wchar_t* argv[]) {
    CLI::App app{"MCDK CLI"};
    app.require_subcommand(1);

    // 定义命令 列出环境信息
    app.add_subcommand("envinfo", "列出当前环境信息")->callback([]() {
        ENV_INFO();
    });

    CLI11_PARSE(app, argc, argv);
    return 0;
}