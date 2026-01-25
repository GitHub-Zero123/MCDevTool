#pragma once
#include <string>
#include <vector>
#include <filesystem>
#include <unordered_set>
#include <mutex>
#include <memory>
#include <functional>
#include <nlohmann/json.hpp>
#include <mcdevtool/debug.h>
#include "console.hpp"

namespace mcdk {
    // 热更新监视任务类
    class ReloadWatcherTask : public MCDevTool::Debug::HotReloadWatcherTask {
    public:
        using HotReloadAction = std::function<void(const nlohmann::json&)>;
        using MCDevTool::Debug::HotReloadWatcherTask::HotReloadWatcherTask;

        // 设置控制台输出回调
        void setOutputCallback(ConsoleOutputCallback callback) {
            mOutputCallback = std::move(callback);
        }

        void setHotReloadAction(HotReloadAction action) {
            mHotReloadAction = std::move(action);
        }

        void setModDirs(std::vector<std::filesystem::path>&& modDirs) {
            modRootDirPaths.clear();
            for (const auto& dir : modDirs) {
                modRootDirPaths.insert(dir);
            }
            MCDevTool::Debug::HotReloadWatcherTask::setModDirs(std::move(modDirs));
        }

        // 从文件路径计算Python模块名
        void pyPathToModuleName(const std::filesystem::path& filePath, std::string& outModuleName) {
            std::filesystem::path cur = filePath;
            std::filesystem::path manifestDir;

            // 向上查找 manifest.json
            while (true) {
                if (std::filesystem::exists(cur / "manifest.json")) {
                    manifestDir = cur;
                    break;
                }

                auto parent = cur.parent_path();
                if (parent == cur) {
                    return; // 没找到 manifest
                }
                cur = parent;
                if(modRootDirPaths.size() > 0 && modRootDirPaths.find(cur) != modRootDirPaths.end()) {
                    // 达到用户指定的mod目录上限
                    return;
                }
            }

            // 计算 filePath 相对于 manifestDir 的路径
            std::filesystem::path rel = std::filesystem::relative(filePath, manifestDir);
            if (rel.empty()) {
                return;
            }

            std::vector<std::string> parts;
            for (const auto& p : rel) {
                parts.push_back(p.string());
            }

            if (parts.empty()) { return; }

            std::string& last = parts.back();
            if (last.size() > 3 && last.ends_with(".py")) {
                last.resize(last.size() - 3);
            }

            // 拼接模块名
            std::string moduleName;
            for (size_t i = 0; i < parts.size(); ++i) {
                moduleName += parts[i];
                if (i + 1 < parts.size()) {
                    moduleName.push_back('.');
                }
            }
            outModuleName = std::move(moduleName);
        }

        void onHotReloadTriggered() override {
            nlohmann::json targetPaths = nlohmann::json::array();
            {
                std::lock_guard<std::mutex> lock(gMutex);
                for(const auto& modulePath : mCachedPyModulePaths) {
                    std::string moduleName;
                    pyPathToModuleName(modulePath, moduleName);
                    if(moduleName.empty()) {
                        continue;
                    }
                    targetPaths.push_back(std::move(moduleName));
                }
                mCachedPyModulePaths.clear();
            }
            if(targetPaths.empty()) {
                return;
            }
            if(mOutputCallback) {
                mOutputCallback("[HotReload] 检测到修改，已触发热更新。", mcdk::ConsoleColor::Yellow);
            }
            // mIpcServer->sendMessage(2, targetPaths.dump()); // FAST RELOAD
            mHotReloadAction(targetPaths);
        }

        void onFileChanged(const std::filesystem::path& filePath) override {
            // 输出变更文件路径
            auto u8Path = filePath.generic_u8string();
            if(mOutputCallback) {
                mOutputCallback("[HotReload] Detected change in: " + std::string(u8Path.begin(), u8Path.end()), mcdk::ConsoleColor::Yellow);
            }
            std::lock_guard<std::mutex> lock(gMutex);
            mCachedPyModulePaths.insert(filePath);
        }

        // void bindServer(const std::shared_ptr<MCDevTool::Debug::DebugIPCServer>& server) {
        //     mIpcServer = server;
        // }

    private:
        // std::shared_ptr<MCDevTool::Debug::DebugIPCServer> mIpcServer;
        HotReloadAction mHotReloadAction;
        std::unordered_set<std::filesystem::path> mCachedPyModulePaths;
        std::unordered_set<std::filesystem::path> modRootDirPaths;
        std::mutex gMutex;
        ConsoleOutputCallback mOutputCallback;
    };

} // namespace mcdk
