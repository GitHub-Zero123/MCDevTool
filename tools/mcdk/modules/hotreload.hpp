#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include <mcdevtool/debug.h>
#include <mcdevtool/utils.h>
#include <nlohmann/json.hpp>

#include "console.hpp"
#include "json_diagnostics.hpp"

namespace mcdk {

    class ConsoleWatcherTask : public MCDevTool::Debug::HotReloadWatcherTask {
    public:
        using MCDevTool::Debug::HotReloadWatcherTask::HotReloadWatcherTask;

        void setOutputCallback(ConsoleOutputCallback callback) { mOutputCallback = std::move(callback); }

    protected:
        void output(ConsoleColor color, const std::string& message) const {
            if (mOutputCallback) {
                mOutputCallback(message, color);
            }
        }

        void outputChangedPath(const std::filesystem::path& filePath) const {
            auto u8Path = filePath.generic_u8string();
            output(ConsoleColor::Yellow, "[HotReload] Detected change in: " + std::string(u8Path.begin(), u8Path.end()));
        }

    private:
        ConsoleOutputCallback mOutputCallback;
    };

    class PyReloadWatcherTask : public ConsoleWatcherTask {
    public:
        using HotReloadAction = std::function<void(const nlohmann::json&)>;
        using ConsoleWatcherTask::ConsoleWatcherTask;

        void setHotReloadAction(HotReloadAction action) { mHotReloadAction = std::move(action); }

        void setModDirs(std::vector<std::filesystem::path>&& modDirs) {
            modRootDirPaths.clear();
            for (const auto& dir : modDirs) {
                modRootDirPaths.insert(dir);
            }
            MCDevTool::Debug::HotReloadWatcherTask::setModDirs(std::move(modDirs));
        }

        bool shouldWatchFile(const std::filesystem::path& filePath) const override {
            return filePath.extension().string() == ".py";
        }

        void onFileChanged(const std::filesystem::path& filePath) override {
            outputChangedPath(filePath);
            std::lock_guard<std::mutex> lock(mMutex);
            mCachedPyModulePaths.insert(filePath);
        }

        void onHotReloadTriggered() override {
            nlohmann::json targetPaths = nlohmann::json::array();
            {
                std::lock_guard<std::mutex> lock(mMutex);
                for (const auto& modulePath : mCachedPyModulePaths) {
                    std::string moduleName;
                    pyPathToModuleName(modulePath, moduleName);
                    if (!moduleName.empty()) {
                        targetPaths.push_back(std::move(moduleName));
                    }
                }
                mCachedPyModulePaths.clear();
            }
            if (targetPaths.empty()) {
                return;
            }
            output(ConsoleColor::Yellow, "[HotReload] 检测到修改，已触发热更新。");
            if (mHotReloadAction) {
                mHotReloadAction(targetPaths);
            }
        }

    private:
        void pyPathToModuleName(const std::filesystem::path& filePath, std::string& outModuleName) {
            std::filesystem::path cur = filePath;
            std::filesystem::path manifestDir;

            while (true) {
                if (std::filesystem::exists(cur / "manifest.json")) {
                    manifestDir = cur;
                    break;
                }

                auto parent = cur.parent_path();
                if (parent == cur) {
                    return;
                }
                cur = parent;
                if (!modRootDirPaths.empty() && modRootDirPaths.find(cur) != modRootDirPaths.end()) {
                    return;
                }
            }

            std::filesystem::path rel = std::filesystem::relative(filePath, manifestDir);
            if (rel.empty()) {
                return;
            }

            std::vector<std::string> parts;
            for (const auto& p : rel) {
                parts.push_back(MCDevTool::Utils::pathToUtf8(p));
            }
            if (parts.empty()) {
                return;
            }

            std::string& last = parts.back();
            if (last.size() > 3 && last.ends_with(".py")) {
                last.resize(last.size() - 3);
            }

            std::string moduleName;
            for (size_t i = 0; i < parts.size(); ++i) {
                moduleName += parts[i];
                if (i + 1 < parts.size()) {
                    moduleName.push_back('.');
                }
            }
            outModuleName = std::move(moduleName);
        }

        HotReloadAction                           mHotReloadAction;
        std::unordered_set<std::filesystem::path> mCachedPyModulePaths;
        std::unordered_set<std::filesystem::path> modRootDirPaths;
        std::mutex                                mMutex;
    };

    class UiReloadWatcherTask : public ConsoleWatcherTask {
    public:
        using UiHotReloadAction = std::function<void()>;
        using ConsoleWatcherTask::ConsoleWatcherTask;

        void setUiHotReloadAction(UiHotReloadAction action) { mUiHotReloadAction = std::move(action); }

        void setModDirs(std::vector<std::filesystem::path>&& modDirs) {
            MCDevTool::Debug::HotReloadWatcherTask::setModDirs(std::move(modDirs));
        }

        bool shouldWatchFile(const std::filesystem::path& filePath) const override {
            return isUiJsonPath(filePath);
        }

        void onFileChanged(const std::filesystem::path& filePath) override {
            outputChangedPath(filePath);
            std::lock_guard<std::mutex> lock(mMutex);
            mDirty = true;
        }

        void onHotReloadTriggered() override {
            {
                std::lock_guard<std::mutex> lock(mMutex);
                if (!mDirty) {
                    return;
                }
                mDirty = false;
            }
            output(ConsoleColor::Yellow, "[HotReload] Detected JSON UI changes; triggering UI hot reload.");
            if (mUiHotReloadAction) {
                mUiHotReloadAction();
            }
        }

    private:
        bool isValidJsonFile(const std::filesystem::path& filePath) const {
            auto diagnostic = json_diagnostics::validateJsonFileWithComments(
                filePath,
                "[HotReload] warning: invalid JSON; UI hot reload skipped"
            );
            if (diagnostic.ok || diagnostic.empty) {
                return true;
            }
            if (!diagnostic.readable) {
                output(
                    ConsoleColor::Yellow,
                    "[HotReload] warning: UI json hot reload skipped: " + diagnostic.message + " "
                        + diagnostic.path
                );
                return false;
            }
            output(ConsoleColor::Yellow, diagnostic.formatted);
            return false;
        }

        bool isUiJsonPath(const std::filesystem::path& filePath) const {
            if (filePath.extension().string() != ".json") {
                return false;
            }
            const auto absPath = std::filesystem::absolute(filePath).lexically_normal();
            return isValidJsonFile(absPath);
        }

        UiHotReloadAction                         mUiHotReloadAction;
        mutable std::mutex                        mMutex;
        bool                                      mDirty = false;
    };

    class ShaderReloadWatcherTask : public ConsoleWatcherTask {
    public:
        using ShaderHotReloadAction = std::function<void(const nlohmann::json&)>;
        using ConsoleWatcherTask::ConsoleWatcherTask;

        void setShaderHotReloadAction(ShaderHotReloadAction action) { mShaderHotReloadAction = std::move(action); }

        void setModDirs(std::vector<std::filesystem::path>&& shaderDirs) {
            mShaderDirs.clear();
            for (const auto& dir : shaderDirs) {
                mShaderDirs.push_back(std::filesystem::absolute(dir).lexically_normal());
            }
            MCDevTool::Debug::HotReloadWatcherTask::setModDirs(std::move(shaderDirs));
        }

        bool shouldWatchFile(const std::filesystem::path& filePath) const override {
            const auto absPath = std::filesystem::absolute(filePath).lexically_normal();
            std::error_code ec;
            return std::filesystem::is_regular_file(absPath, ec);
        }

        void onFileChanged(const std::filesystem::path& filePath) override {
            outputChangedPath(filePath);
            auto shaderName = shaderPathToReloadName(filePath);
            if (shaderName.empty()) {
                return;
            }
            std::lock_guard<std::mutex> lock(mMutex);
            mCachedShaderNames.insert(std::move(shaderName));
        }

        void onHotReloadTriggered() override {
            nlohmann::json shaderNames = nlohmann::json::array();
            {
                std::lock_guard<std::mutex> lock(mMutex);
                for (const auto& shaderName : mCachedShaderNames) {
                    shaderNames.push_back(shaderName);
                }
                mCachedShaderNames.clear();
            }
            if (shaderNames.empty()) {
                return;
            }
            output(ConsoleColor::Yellow, "[HotReload] Detected shader changes; triggering shader hot reload.");
            if (mShaderHotReloadAction) {
                mShaderHotReloadAction(shaderNames);
            }
        }

    private:
        std::string shaderPathToReloadName(const std::filesystem::path& filePath) const {
            const auto absPath = std::filesystem::absolute(filePath).lexically_normal();
            for (const auto& shaderDir : mShaderDirs) {
                if (!isPathInsideDir(absPath, shaderDir)) {
                    continue;
                }
                auto relPath = std::filesystem::relative(absPath, shaderDir);
                if (relPath.empty() || relPath == ".") {
                    return {};
                }
                return MCDevTool::Utils::pathToGenericUtf8(relPath);
            }
            return {};
        }

        static bool isPathInsideDir(const std::filesystem::path& child, const std::filesystem::path& parent) {
            auto childIt  = child.begin();
            auto parentIt = parent.begin();
            for (; parentIt != parent.end(); ++parentIt, ++childIt) {
                if (childIt == child.end() || *childIt != *parentIt) {
                    return false;
                }
            }
            return true;
        }

        ShaderHotReloadAction                    mShaderHotReloadAction;
        std::vector<std::filesystem::path>       mShaderDirs;
        std::unordered_set<std::string>          mCachedShaderNames;
        mutable std::mutex                       mMutex;
    };

} // namespace mcdk
