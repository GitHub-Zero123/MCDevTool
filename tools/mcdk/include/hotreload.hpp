#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include <mcdevtool/debug.h>

#include "console.hpp"

namespace mcdk {

    using ReloadNames  = std::vector<std::string>;
    using ReloadAction = std::function<void(const ReloadNames&)>;

    class ConsoleWatcherTask : public MCDevTool::Debug::HotReloadWatcherTask {
    public:
        using MCDevTool::Debug::HotReloadWatcherTask::HotReloadWatcherTask;

        void setOutputCallback(ConsoleOutputCallback callback);

    protected:
        void output(ConsoleColor color, const std::string& message) const;
        void outputChangedPath(const std::filesystem::path& filePath) const;

        [[nodiscard]] bool isValidHotReloadJsonFile(
            const std::filesystem::path& filePath,
            const std::string&           invalidTitle,
            const std::string&           unreadablePrefix
        ) const;

    private:
        ConsoleOutputCallback mOutputCallback;
    };

    class PyReloadWatcherTask : public ConsoleWatcherTask {
    public:
        using HotReloadAction = ReloadAction;
        using ConsoleWatcherTask::ConsoleWatcherTask;

        void setHotReloadAction(HotReloadAction action);
        void setModDirs(std::vector<std::filesystem::path>&& modDirectories);

        [[nodiscard]] bool shouldWatchFile(const std::filesystem::path& filePath) const override;
        void               onFileChanged(const std::filesystem::path& filePath) override;
        void               onHotReloadTriggered() override;

    private:
        void pyPathToModuleName(const std::filesystem::path& filePath, std::string& outModuleName) const;

        HotReloadAction                           mHotReloadAction;
        std::unordered_set<std::filesystem::path> mCachedPyModulePaths;
        std::unordered_set<std::filesystem::path> mModRootDirectoryPaths;
        std::unordered_set<std::filesystem::path> mModPackageDirectoryPaths;
        std::mutex                                mMutex;
    };

    class UiReloadWatcherTask : public ConsoleWatcherTask {
    public:
        using UiHotReloadAction = std::function<void()>;
        using ConsoleWatcherTask::ConsoleWatcherTask;

        void setUiHotReloadAction(UiHotReloadAction action);
        void setModDirs(std::vector<std::filesystem::path>&& modDirectories);

        [[nodiscard]] bool shouldWatchFile(const std::filesystem::path& filePath) const override;
        void               onFileChanged(const std::filesystem::path& filePath) override;
        void               onHotReloadTriggered() override;

    private:
        [[nodiscard]] bool isValidJsonFile(const std::filesystem::path& filePath) const;

        UiHotReloadAction  mUiHotReloadAction;
        mutable std::mutex mMutex;
        bool               mDirty = false;
    };

    class IncrementalReloadWatcherTask : public ConsoleWatcherTask {
    public:
        using ConsoleWatcherTask::ConsoleWatcherTask;

        void setModDirs(std::vector<std::filesystem::path>&& rootDirectories);
        void onFileChanged(const std::filesystem::path& filePath) override;
        void onHotReloadTriggered() override;

    protected:
        void setReloadAction(ReloadAction action);

        [[nodiscard]] virtual bool        acceptChangedFile(const std::filesystem::path& absolutePath) const;
        [[nodiscard]] virtual std::string reloadNamePrefix() const;
        [[nodiscard]] virtual std::string triggeredMessage() const = 0;
        [[nodiscard]] bool                isRegularFile(const std::filesystem::path& absolutePath) const;

    private:
        [[nodiscard]] std::string pathToReloadName(const std::filesystem::path& filePath) const;
        [[nodiscard]] static bool
        isPathInsideDirectory(const std::filesystem::path& child, const std::filesystem::path& parent);

        ReloadAction                       mReloadAction;
        std::vector<std::filesystem::path> mRootDirectories;
        std::unordered_set<std::string>    mCachedReloadNames;
        mutable std::mutex                 mMutex;
    };

    class ShaderReloadWatcherTask : public IncrementalReloadWatcherTask {
    public:
        using ShaderHotReloadAction = ReloadAction;
        using IncrementalReloadWatcherTask::IncrementalReloadWatcherTask;

        void               setShaderHotReloadAction(ShaderHotReloadAction action);
        [[nodiscard]] bool shouldWatchFile(const std::filesystem::path& filePath) const override;

    protected:
        [[nodiscard]] std::string triggeredMessage() const override;
    };

    class MaterialReloadWatcherTask : public IncrementalReloadWatcherTask {
    public:
        using MaterialHotReloadAction = ReloadAction;
        using IncrementalReloadWatcherTask::IncrementalReloadWatcherTask;

        void               setMaterialHotReloadAction(MaterialHotReloadAction action);
        [[nodiscard]] bool shouldWatchFile(const std::filesystem::path& filePath) const override;

    protected:
        [[nodiscard]] bool        acceptChangedFile(const std::filesystem::path& absolutePath) const override;
        [[nodiscard]] std::string reloadNamePrefix() const override;
        [[nodiscard]] std::string triggeredMessage() const override;
    };

    class ParticleReloadWatcherTask : public IncrementalReloadWatcherTask {
    public:
        using ParticleHotReloadAction = ReloadAction;
        using IncrementalReloadWatcherTask::IncrementalReloadWatcherTask;

        void               setParticleHotReloadAction(ParticleHotReloadAction action);
        [[nodiscard]] bool shouldWatchFile(const std::filesystem::path& filePath) const override;

    protected:
        [[nodiscard]] bool        acceptChangedFile(const std::filesystem::path& absolutePath) const override;
        [[nodiscard]] std::string reloadNamePrefix() const override;
        [[nodiscard]] std::string triggeredMessage() const override;
    };

} // namespace mcdk
