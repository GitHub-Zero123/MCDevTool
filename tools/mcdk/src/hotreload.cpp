#include <mcdk/hotreload.hpp>

#include <system_error>
#include <utility>

#include <mcdevtool/utils.h>

#include <mcdk/json_diagnostics.hpp>

namespace mcdk {

    void ConsoleWatcherTask::setOutputCallback(ConsoleOutputCallback callback) {
        mOutputCallback = std::move(callback);
    }

    void ConsoleWatcherTask::output(ConsoleColor color, const std::string& message) const {
        if (mOutputCallback) {
            mOutputCallback(message, color);
        }
    }

    void ConsoleWatcherTask::outputChangedPath(const std::filesystem::path& filePath) const {
        output(
            ConsoleColor::Yellow,
            "[HotReload] Detected change in: " + MCDevTool::Utils::pathToGenericUtf8(filePath)
        );
    }

    bool ConsoleWatcherTask::isValidHotReloadJsonFile(
        const std::filesystem::path& filePath,
        const std::string&           invalidTitle,
        const std::string&           unreadablePrefix
    ) const {
        const auto diagnostic = json_diagnostics::validateJsonFileWithComments(filePath, invalidTitle);
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

    void PyReloadWatcherTask::setHotReloadAction(HotReloadAction action) { mHotReloadAction = std::move(action); }

    void PyReloadWatcherTask::setModDirs(std::vector<std::filesystem::path>&& modDirectories) {
        mModRootDirectoryPaths.clear();
        mModPackageDirectoryPaths.clear();
        for (const auto& directory : modDirectories) {
            const auto rootDirectory = std::filesystem::absolute(directory).lexically_normal();
            mModRootDirectoryPaths.insert(rootDirectory);

            std::error_code                               error;
            std::filesystem::recursive_directory_iterator iterator(
                rootDirectory,
                std::filesystem::directory_options::skip_permission_denied,
                error
            );
            const std::filesystem::recursive_directory_iterator end;
            while (!error && iterator != end) {
                const auto&     entry = *iterator;
                std::error_code entryError;
                if (entry.is_regular_file(entryError) && entry.path().filename() == "modMain.py") {
                    mModPackageDirectoryPaths.insert(entry.path().parent_path().lexically_normal());
                }
                iterator.increment(error);
            }
        }
        MCDevTool::Debug::HotReloadWatcherTask::setModDirs(std::move(modDirectories));
    }

    bool PyReloadWatcherTask::shouldWatchFile(const std::filesystem::path& filePath) const {
        if (filePath.extension() != ".py") {
            return false;
        }
        const auto normalizedPath = std::filesystem::absolute(filePath).lexically_normal();
        for (const auto& packageDirectory : mModPackageDirectoryPaths) {
            const auto relativePath = normalizedPath.lexically_relative(packageDirectory);
            if (!relativePath.empty() && *relativePath.begin() != "..") {
                return true;
            }
        }
        return false;
    }

    void PyReloadWatcherTask::onFileChanged(const std::filesystem::path& filePath) {
        outputChangedPath(filePath);
        std::lock_guard lock(mMutex);
        mCachedPyModulePaths.insert(filePath);
    }

    void PyReloadWatcherTask::onHotReloadTriggered() {
        std::unordered_set<std::filesystem::path> changedPaths;
        {
            std::lock_guard lock(mMutex);
            // Swap in O(1) so filesystem traversal never blocks the file-change producer under this mutex.
            changedPaths.swap(mCachedPyModulePaths);
        }

        ReloadNames targetModules;
        targetModules.reserve(changedPaths.size());
        for (const auto& modulePath : changedPaths) {
            std::string moduleName;
            pyPathToModuleName(modulePath, moduleName);
            if (!moduleName.empty()) {
                targetModules.push_back(std::move(moduleName));
            }
        }
        if (targetModules.empty()) {
            return;
        }
        output(ConsoleColor::Yellow, "[HotReload] 检测到修改，已触发热更新。");
        if (mHotReloadAction) {
            mHotReloadAction(targetModules);
        }
    }

    void
    PyReloadWatcherTask::pyPathToModuleName(const std::filesystem::path& filePath, std::string& outModuleName) const {
        auto                  current = filePath;
        std::filesystem::path manifestDirectory;
        while (true) {
            if (std::filesystem::exists(current / "manifest.json")) {
                manifestDirectory = current;
                break;
            }
            const auto parent = current.parent_path();
            if (parent == current) {
                return;
            }
            current = parent;
            if (!mModRootDirectoryPaths.empty() && mModRootDirectoryPaths.contains(current)) {
                return;
            }
        }

        const auto relativePath = std::filesystem::relative(filePath, manifestDirectory);
        if (relativePath.empty()) {
            return;
        }
        std::vector<std::string> parts;
        for (const auto& part : relativePath) {
            parts.push_back(MCDevTool::Utils::pathToUtf8(part));
        }
        if (parts.empty()) {
            return;
        }
        if (auto& fileName = parts.back(); fileName.size() > 3 && fileName.ends_with(".py")) {
            fileName.resize(fileName.size() - 3);
        }

        outModuleName.clear();
        for (std::size_t index = 0; index < parts.size(); ++index) {
            outModuleName += parts[index];
            if (index + 1 < parts.size()) {
                outModuleName.push_back('.');
            }
        }
    }

    void UiReloadWatcherTask::setUiHotReloadAction(UiHotReloadAction action) { mUiHotReloadAction = std::move(action); }

    void UiReloadWatcherTask::setModDirs(std::vector<std::filesystem::path>&& modDirectories) {
        MCDevTool::Debug::HotReloadWatcherTask::setModDirs(std::move(modDirectories));
    }

    bool UiReloadWatcherTask::shouldWatchFile(const std::filesystem::path& filePath) const {
        return filePath.extension() == ".json";
    }

    void UiReloadWatcherTask::onFileChanged(const std::filesystem::path& filePath) {
        const auto absolutePath = std::filesystem::absolute(filePath).lexically_normal();
        if (!isValidJsonFile(absolutePath)) {
            return;
        }
        outputChangedPath(filePath);
        std::lock_guard lock(mMutex);
        mDirty = true;
    }

    void UiReloadWatcherTask::onHotReloadTriggered() {
        {
            std::lock_guard lock(mMutex);
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

    bool UiReloadWatcherTask::isValidJsonFile(const std::filesystem::path& filePath) const {
        return isValidHotReloadJsonFile(
            filePath,
            "[HotReload] warning: invalid JSON; UI hot reload skipped",
            "[HotReload] warning: UI json hot reload skipped: "
        );
    }

    void IncrementalReloadWatcherTask::setModDirs(std::vector<std::filesystem::path>&& rootDirectories) {
        mRootDirectories.clear();
        mRootDirectories.reserve(rootDirectories.size());
        for (const auto& directory : rootDirectories) {
            mRootDirectories.push_back(std::filesystem::absolute(directory).lexically_normal());
        }
        MCDevTool::Debug::HotReloadWatcherTask::setModDirs(std::move(rootDirectories));
    }

    void IncrementalReloadWatcherTask::onFileChanged(const std::filesystem::path& filePath) {
        const auto absolutePath = std::filesystem::absolute(filePath).lexically_normal();
        if (!acceptChangedFile(absolutePath)) {
            return;
        }
        outputChangedPath(filePath);
        auto reloadName = pathToReloadName(absolutePath);
        if (reloadName.empty()) {
            return;
        }
        std::lock_guard lock(mMutex);
        mCachedReloadNames.insert(std::move(reloadName));
    }

    void IncrementalReloadWatcherTask::onHotReloadTriggered() {
        ReloadNames reloadNames;
        {
            std::lock_guard lock(mMutex);
            reloadNames.assign(mCachedReloadNames.begin(), mCachedReloadNames.end());
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

    void IncrementalReloadWatcherTask::setReloadAction(ReloadAction action) { mReloadAction = std::move(action); }

    bool IncrementalReloadWatcherTask::acceptChangedFile(const std::filesystem::path&) const { return true; }

    std::string IncrementalReloadWatcherTask::reloadNamePrefix() const { return {}; }

    bool IncrementalReloadWatcherTask::isRegularFile(const std::filesystem::path& absolutePath) const {
        std::error_code error;
        return std::filesystem::is_regular_file(absolutePath, error);
    }

    std::string IncrementalReloadWatcherTask::pathToReloadName(const std::filesystem::path& filePath) const {
        const auto absolutePath = std::filesystem::absolute(filePath).lexically_normal();
        for (const auto& rootDirectory : mRootDirectories) {
            if (!isPathInsideDirectory(absolutePath, rootDirectory)) {
                continue;
            }
            const auto relativePath = std::filesystem::relative(absolutePath, rootDirectory);
            if (relativePath.empty() || relativePath == ".") {
                return {};
            }
            return reloadNamePrefix() + MCDevTool::Utils::pathToGenericUtf8(relativePath);
        }
        return {};
    }

    bool IncrementalReloadWatcherTask::isPathInsideDirectory(
        const std::filesystem::path& child,
        const std::filesystem::path& parent
    ) {
        auto childIterator  = child.begin();
        auto parentIterator = parent.begin();
        for (; parentIterator != parent.end(); ++parentIterator, ++childIterator) {
            if (childIterator == child.end() || *childIterator != *parentIterator) {
                return false;
            }
        }
        return true;
    }

    void ShaderReloadWatcherTask::setShaderHotReloadAction(ShaderHotReloadAction action) {
        setReloadAction(std::move(action));
    }

    bool ShaderReloadWatcherTask::shouldWatchFile(const std::filesystem::path& filePath) const {
        return isRegularFile(std::filesystem::absolute(filePath).lexically_normal());
    }

    std::string ShaderReloadWatcherTask::triggeredMessage() const {
        return "[HotReload] Detected shader changes; triggering shader hot reload.";
    }

    void MaterialReloadWatcherTask::setMaterialHotReloadAction(MaterialHotReloadAction action) {
        setReloadAction(std::move(action));
    }

    bool MaterialReloadWatcherTask::shouldWatchFile(const std::filesystem::path& filePath) const {
        return isRegularFile(std::filesystem::absolute(filePath).lexically_normal());
    }

    bool MaterialReloadWatcherTask::acceptChangedFile(const std::filesystem::path& absolutePath) const {
        return isValidHotReloadJsonFile(
            absolutePath,
            "[HotReload] warning: invalid Material JSON; material hot reload skipped",
            "[HotReload] warning: material hot reload skipped: "
        );
    }

    std::string MaterialReloadWatcherTask::reloadNamePrefix() const { return "materials/"; }

    std::string MaterialReloadWatcherTask::triggeredMessage() const {
        return "[HotReload] Detected material changes; triggering material hot reload.";
    }

    void ParticleReloadWatcherTask::setParticleHotReloadAction(ParticleHotReloadAction action) {
        setReloadAction(std::move(action));
    }

    bool ParticleReloadWatcherTask::shouldWatchFile(const std::filesystem::path& filePath) const {
        return filePath.extension() == ".json" && isRegularFile(std::filesystem::absolute(filePath).lexically_normal());
    }

    bool ParticleReloadWatcherTask::acceptChangedFile(const std::filesystem::path& absolutePath) const {
        return isValidHotReloadJsonFile(
            absolutePath,
            "[HotReload] warning: invalid Particle JSON; particle hot reload skipped",
            "[HotReload] warning: particle hot reload skipped: "
        );
    }

    std::string ParticleReloadWatcherTask::reloadNamePrefix() const { return "particles/"; }

    std::string ParticleReloadWatcherTask::triggeredMessage() const {
        return "[HotReload] Detected particle changes; triggering particle hot reload.";
    }

} // namespace mcdk
