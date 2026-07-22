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

        // JSON 语法校验，异常时打印诊断信息并跳过本次热更新
        bool isValidHotReloadJsonFile(
            const std::filesystem::path& filePath,
            const std::string&           invalidTitle,
            const std::string&           unreadablePrefix
        ) const {
            auto diagnostic = json_diagnostics::validateJsonFileWithComments(filePath, invalidTitle);
            if (diagnostic.ok || diagnostic.empty) {
                return true;
            }
            if (!diagnostic.readable) {
                output(ConsoleColor::Yellow, unreadablePrefix + diagnostic.message + " " + diagnostic.path);
                return false;
            }
            output(ConsoleColor::Yellow, diagnostic.formatted);
            return false;
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
            modPackageDirPaths.clear();
            for (const auto& dir : modDirs) {
                const auto rootDir = std::filesystem::absolute(dir).lexically_normal();
                modRootDirPaths.insert(rootDir);

                std::error_code ec;
                std::filesystem::recursive_directory_iterator it(
                    rootDir,
                    std::filesystem::directory_options::skip_permission_denied,
                    ec
                );
                const std::filesystem::recursive_directory_iterator end;
                while (!ec && it != end) {
                    const auto& entry = *it;
                    std::error_code entryError;
                    if (entry.is_regular_file(entryError) && entry.path().filename() == "modMain.py") {
                        modPackageDirPaths.insert(entry.path().parent_path().lexically_normal());
                    }
                    it.increment(ec);
                }
            }
            MCDevTool::Debug::HotReloadWatcherTask::setModDirs(std::move(modDirs));
        }

        bool shouldWatchFile(const std::filesystem::path& filePath) const override {
            if (filePath.extension() != ".py") {
                return false;
            }

            const auto normalizedPath = std::filesystem::absolute(filePath).lexically_normal();
            for (const auto& packageDir : modPackageDirPaths) {
                const auto relativePath = normalizedPath.lexically_relative(packageDir);
                if (!relativePath.empty() && *relativePath.begin() != "..") {
                    return true;
                }
            }
            return false;
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
        std::unordered_set<std::filesystem::path> modPackageDirPaths;
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
            return filePath.extension().string() == ".json";
        }

        void onFileChanged(const std::filesystem::path& filePath) override {
            // 校验放在防抖之后，避免同一次保存的多条通知重复输出 JSON 诊断
            const auto absPath = std::filesystem::absolute(filePath).lexically_normal();
            if (!isValidJsonFile(absPath)) {
                return;
            }
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
            return isValidHotReloadJsonFile(
                filePath,
                "[HotReload] warning: invalid JSON; UI hot reload skipped",
                "[HotReload] warning: UI json hot reload skipped: "
            );
        }

        UiHotReloadAction                         mUiHotReloadAction;
        mutable std::mutex                        mMutex;
        bool                                      mDirty = false;
    };

    // 增量热更新任务基类：缓存变更文件相对于监听根目录的重载名，触发时一次性提交
    class IncrementalReloadWatcherTask : public ConsoleWatcherTask {
    public:
        using ReloadAction = std::function<void(const nlohmann::json&)>;
        using ConsoleWatcherTask::ConsoleWatcherTask;

        void setModDirs(std::vector<std::filesystem::path>&& rootDirs) {
            mRootDirs.clear();
            for (const auto& dir : rootDirs) {
                mRootDirs.push_back(std::filesystem::absolute(dir).lexically_normal());
            }
            MCDevTool::Debug::HotReloadWatcherTask::setModDirs(std::move(rootDirs));
        }

        void onFileChanged(const std::filesystem::path& filePath) override {
            const auto absPath = std::filesystem::absolute(filePath).lexically_normal();
            // 内容校验放在此处而非 shouldWatchFile：谓词会被同一次保存的多条通知重复调用，
            // 而 onFileChanged 已经过防抖，每次真实变更只会执行一次
            if (!acceptChangedFile(absPath)) {
                return;
            }
            outputChangedPath(filePath);
            auto reloadName = pathToReloadName(absPath);
            if (reloadName.empty()) {
                return;
            }
            std::lock_guard<std::mutex> lock(mMutex);
            mCachedReloadNames.insert(std::move(reloadName));
        }

        void onHotReloadTriggered() override {
            nlohmann::json reloadNames = nlohmann::json::array();
            {
                std::lock_guard<std::mutex> lock(mMutex);
                for (const auto& reloadName : mCachedReloadNames) {
                    reloadNames.push_back(reloadName);
                }
                mCachedReloadNames.clear();
            }
            if (reloadNames.empty()) {
                return;
            }
            output(ConsoleColor::Yellow, triggeredMessage());
            if (mReloadAction) {
                mReloadAction(reloadNames);
            }
        }

    protected:
        void setReloadAction(ReloadAction action) { mReloadAction = std::move(action); }

        // 变更文件的内容准入检查（已在防抖之后），可在此输出诊断
        virtual bool acceptChangedFile(const std::filesystem::path& absPath) const { return true; }

        // 重载名前缀，例如 material 为 "materials/"；shader 无前缀
        virtual std::string reloadNamePrefix() const { return {}; }

        // 触发热更新时打印的提示
        virtual std::string triggeredMessage() const = 0;

        bool isRegularFile(const std::filesystem::path& absPath) const {
            std::error_code ec;
            return std::filesystem::is_regular_file(absPath, ec);
        }

    private:
        std::string pathToReloadName(const std::filesystem::path& filePath) const {
            const auto absPath = std::filesystem::absolute(filePath).lexically_normal();
            for (const auto& rootDir : mRootDirs) {
                if (!isPathInsideDir(absPath, rootDir)) {
                    continue;
                }
                auto relPath = std::filesystem::relative(absPath, rootDir);
                if (relPath.empty() || relPath == ".") {
                    return {};
                }
                return reloadNamePrefix() + MCDevTool::Utils::pathToGenericUtf8(relPath);
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

        ReloadAction                             mReloadAction;
        std::vector<std::filesystem::path>       mRootDirs;
        std::unordered_set<std::string>          mCachedReloadNames;
        mutable std::mutex                       mMutex;
    };

    class ShaderReloadWatcherTask : public IncrementalReloadWatcherTask {
    public:
        using ShaderHotReloadAction = std::function<void(const nlohmann::json&)>;
        using IncrementalReloadWatcherTask::IncrementalReloadWatcherTask;

        void setShaderHotReloadAction(ShaderHotReloadAction action) { setReloadAction(std::move(action)); }

        bool shouldWatchFile(const std::filesystem::path& filePath) const override {
            return isRegularFile(std::filesystem::absolute(filePath).lexically_normal());
        }

    protected:
        std::string triggeredMessage() const override {
            return "[HotReload] Detected shader changes; triggering shader hot reload.";
        }
    };

    class MaterialReloadWatcherTask : public IncrementalReloadWatcherTask {
    public:
        using MaterialHotReloadAction = std::function<void(const nlohmann::json&)>;
        using IncrementalReloadWatcherTask::IncrementalReloadWatcherTask;

        void setMaterialHotReloadAction(MaterialHotReloadAction action) { setReloadAction(std::move(action)); }

        bool shouldWatchFile(const std::filesystem::path& filePath) const override {
            return isRegularFile(std::filesystem::absolute(filePath).lexically_normal());
        }

    protected:
        bool acceptChangedFile(const std::filesystem::path& absPath) const override {
            return isValidHotReloadJsonFile(
                absPath,
                "[HotReload] warning: invalid Material JSON; material hot reload skipped",
                "[HotReload] warning: material hot reload skipped: "
            );
        }

        std::string reloadNamePrefix() const override { return "materials/"; }

        std::string triggeredMessage() const override {
            return "[HotReload] Detected material changes; triggering material hot reload.";
        }
    };

    class ParticleReloadWatcherTask : public IncrementalReloadWatcherTask {
    public:
        using ParticleHotReloadAction = std::function<void(const nlohmann::json&)>;
        using IncrementalReloadWatcherTask::IncrementalReloadWatcherTask;

        void setParticleHotReloadAction(ParticleHotReloadAction action) { setReloadAction(std::move(action)); }

        bool shouldWatchFile(const std::filesystem::path& filePath) const override {
            if (filePath.extension().string() != ".json") {
                return false;
            }
            return isRegularFile(std::filesystem::absolute(filePath).lexically_normal());
        }

    protected:
        bool acceptChangedFile(const std::filesystem::path& absPath) const override {
            return isValidHotReloadJsonFile(
                absPath,
                "[HotReload] warning: invalid Particle JSON; particle hot reload skipped",
                "[HotReload] warning: particle hot reload skipped: "
            );
        }

        std::string reloadNamePrefix() const override { return "particles/"; }

        std::string triggeredMessage() const override {
            return "[HotReload] Detected particle changes; triggering particle hot reload.";
        }
    };

} // namespace mcdk
