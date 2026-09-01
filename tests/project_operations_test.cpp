#include <mcdk/project_operations.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace {
    namespace fs = std::filesystem;

    void require(bool condition, std::string_view message) {
        if (!condition) {
            throw std::runtime_error(std::string(message));
        }
    }

    struct TemporaryDirectory {
        fs::path path;

        TemporaryDirectory() {
            const auto id = std::chrono::high_resolution_clock::now().time_since_epoch().count();
            path          = fs::temp_directory_path() / ("mcdk-project-operations-test-" + std::to_string(id));
            fs::create_directories(path);
        }

        ~TemporaryDirectory() {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    };

    void writeFile(const fs::path& path, std::string_view content) {
        fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!output) {
            throw std::runtime_error("Unable to write test fixture.");
        }
    }

    [[nodiscard]] std::string readFile(const fs::path& path) {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    [[nodiscard]] std::string pathToUtf8(const fs::path& path) {
        const auto value = path.generic_u8string();
        return {value.begin(), value.end()};
    }

    [[nodiscard]] std::string basicManifest(
        std::string_view name,
        std::string_view moduleType,
        std::string_view headerUuid,
        std::string_view moduleUuid,
        std::string_view version = "[1, 2, 3]"
    ) {
        return "{\n"
               "  \"format_version\": 2,\n"
               "  \"header\": {\"name\": \""
             + std::string(name) + "\", \"uuid\": \"" + std::string(headerUuid)
             + "\", \"version\": " + std::string(version)
             + "},\n"
               "  \"modules\": [{\"type\": \""
             + std::string(moduleType) + "\", \"uuid\": \"" + std::string(moduleUuid)
             + "\", \"version\": " + std::string(version)
             + "}]\n"
               "}\n";
    }

    [[nodiscard]] std::string manifestWithDependency(
        std::string_view name,
        std::string_view headerUuid,
        std::string_view moduleUuid,
        std::string_view dependencyUuid,
        std::string_view version = "[1, 0, 0]"
    ) {
        auto       result  = basicManifest(name, "data", headerUuid, moduleUuid, version);
        const auto closing = result.rfind("}\n");
        result.insert(
            closing,
            ",\n  \"dependencies\": [{\"uuid\": \"" + std::string(dependencyUuid)
                + "\", \"version\": " + std::string(version) + "}]\n"
        );
        return result;
    }

    [[nodiscard]] std::uint64_t rootLockHash(const fs::path& root) {
        auto value = pathToUtf8(root.lexically_normal());
#ifdef _WIN32
        std::ranges::transform(value, value.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
#endif
        std::uint64_t hash = 14695981039346656037ULL;
        for (const unsigned char character : value) {
            hash ^= character;
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    class HeldProjectLock final {
    public:
        explicit HeldProjectLock(const fs::path& root) {
            const auto canonical = fs::canonical(root);
            const auto directory = fs::temp_directory_path() / "mcdk-project-locks";
            fs::create_directories(directory);
            std::ostringstream name;
            name << std::hex << std::setw(16) << std::setfill('0') << rootLockHash(canonical) << ".lock";
            path_ = directory / name.str();
#ifdef _WIN32
            handle_ = CreateFileW(
                path_.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            );
            if (handle_ == INVALID_HANDLE_VALUE) {
                throw std::runtime_error("Unable to acquire the test project lock.");
            }
#else
            descriptor_ = ::open(path_.c_str(), O_CREAT | O_RDWR, 0600);
            if (descriptor_ < 0 || ::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
                throw std::runtime_error("Unable to acquire the test project lock.");
            }
#endif
        }

        ~HeldProjectLock() {
#ifdef _WIN32
            if (handle_ != INVALID_HANDLE_VALUE) {
                CloseHandle(handle_);
                std::error_code ignored;
                fs::remove(path_, ignored);
            }
#else
            if (descriptor_ >= 0) {
                ::flock(descriptor_, LOCK_UN);
                ::close(descriptor_);
                std::error_code ignored;
                fs::remove(path_, ignored);
            }
#endif
        }

        HeldProjectLock(const HeldProjectLock&)            = delete;
        HeldProjectLock& operator=(const HeldProjectLock&) = delete;

    private:
        fs::path path_;
#ifdef _WIN32
        HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
        int descriptor_ = -1;
#endif
    };

#ifdef _WIN32
    class HeldAppendHandle final {
    public:
        explicit HeldAppendHandle(const fs::path& path) {
            handle_ = CreateFileW(
                path.c_str(),
                FILE_APPEND_DATA,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            );
            if (handle_ == INVALID_HANDLE_VALUE) {
                throw std::runtime_error("Unable to open the test manifest append handle.");
            }
        }

        ~HeldAppendHandle() {
            if (handle_ != INVALID_HANDLE_VALUE) {
                CloseHandle(handle_);
            }
        }

        HeldAppendHandle(const HeldAppendHandle&)            = delete;
        HeldAppendHandle& operator=(const HeldAppendHandle&) = delete;

        void append(std::string_view content) {
            LARGE_INTEGER end{};
            if (!SetFilePointerEx(handle_, end, nullptr, FILE_END)) {
                throw std::runtime_error("Unable to seek the test append handle.");
            }
            DWORD written = 0;
            if (!WriteFile(handle_, content.data(), static_cast<DWORD>(content.size()), &written, nullptr)
                || written != content.size()) {
                throw std::runtime_error("Unable to write through the test append handle.");
            }
        }

    private:
        HANDLE handle_ = INVALID_HANDLE_VALUE;
    };

    class HeldNoDeleteShareReadHandle final {
    public:
        explicit HeldNoDeleteShareReadHandle(const fs::path& path) {
            handle_ = CreateFileW(
                path.c_str(),
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            );
            if (handle_ == INVALID_HANDLE_VALUE) {
                throw std::runtime_error("Unable to open the test no-delete-share reader.");
            }
        }

        ~HeldNoDeleteShareReadHandle() {
            if (handle_ != INVALID_HANDLE_VALUE) {
                CloseHandle(handle_);
            }
        }

        HeldNoDeleteShareReadHandle(const HeldNoDeleteShareReadHandle&)            = delete;
        HeldNoDeleteShareReadHandle& operator=(const HeldNoDeleteShareReadHandle&) = delete;

    private:
        HANDLE handle_ = INVALID_HANDLE_VALUE;
    };
#endif

    void setCommitFailure(std::string_view value) {
#ifdef _WIN32
        _putenv_s("MCDK_TEST_FAIL_PROJECT_COMMIT_AFTER", std::string(value).c_str());
#else
        if (value.empty()) {
            unsetenv("MCDK_TEST_FAIL_PROJECT_COMMIT_AFTER");
        } else {
            setenv("MCDK_TEST_FAIL_PROJECT_COMMIT_AFTER", std::string(value).c_str(), 1);
        }
#endif
    }

    void setSourceMutation(std::string_view value) {
#ifdef _WIN32
        _putenv_s("MCDK_TEST_MUTATE_PROJECT_SOURCE_BEFORE_COMMIT", std::string(value).c_str());
#else
        if (value.empty()) {
            unsetenv("MCDK_TEST_MUTATE_PROJECT_SOURCE_BEFORE_COMMIT");
        } else {
            setenv("MCDK_TEST_MUTATE_PROJECT_SOURCE_BEFORE_COMMIT", std::string(value).c_str(), 1);
        }
#endif
    }

    void setSourceMutationAfterFirstClaim(std::string_view value) {
#ifdef _WIN32
        _putenv_s("MCDK_TEST_MUTATE_PROJECT_SOURCE_AFTER_FIRST_CLAIM", std::string(value).c_str());
#else
        if (value.empty()) {
            unsetenv("MCDK_TEST_MUTATE_PROJECT_SOURCE_AFTER_FIRST_CLAIM");
        } else {
            setenv("MCDK_TEST_MUTATE_PROJECT_SOURCE_AFTER_FIRST_CLAIM", std::string(value).c_str(), 1);
        }
#endif
    }

    void setFileGuardProbe(std::string_view value) {
#ifdef _WIN32
        _putenv_s("MCDK_TEST_PROBE_PROJECT_FILE_GUARDS", std::string(value).c_str());
#else
        if (value.empty()) {
            unsetenv("MCDK_TEST_PROBE_PROJECT_FILE_GUARDS");
        } else {
            setenv("MCDK_TEST_PROBE_PROJECT_FILE_GUARDS", std::string(value).c_str(), 1);
        }
#endif
    }

    void setOccupiedBackupInjection(std::string_view value) {
#ifdef _WIN32
        _putenv_s("MCDK_TEST_OCCUPY_PROJECT_BACKUP_BEFORE_CLAIM", std::string(value).c_str());
#else
        if (value.empty()) {
            unsetenv("MCDK_TEST_OCCUPY_PROJECT_BACKUP_BEFORE_CLAIM");
        } else {
            setenv("MCDK_TEST_OCCUPY_PROJECT_BACKUP_BEFORE_CLAIM", std::string(value).c_str(), 1);
        }
#endif
    }

    [[nodiscard]] const mcdk::project::ManifestSummary&
    manifestNamed(const mcdk::project::ProjectSummary& summary, std::string_view name) {
        for (const auto& manifest : summary.manifests) {
            if (manifest.name == name) {
                return manifest;
            }
        }
        throw std::runtime_error("Expected manifest was not discovered.");
    }

    void testDiscoversSingleMultiAndConfiguredPacksWithoutFollowingLinks() {
        TemporaryDirectory temporary;

        const auto singleRoot = temporary.path / "single";
        writeFile(
            singleRoot / "manifest.json",
            basicManifest(
                "Single",
                "data",
                "01010101-0101-4101-8101-010101010101",
                "02020202-0202-4202-8202-020202020202"
            )
        );
        const auto single = mcdk::project::inspectProject(singleRoot);
        require(
            single.kind == mcdk::project::ProjectKind::SinglePack,
            "Root manifest was not identified as a single pack."
        );
        require(single.manifests.size() == 1, "Single-pack discovery returned an unexpected manifest count.");
        require(single.packDirectories.front() == fs::canonical(singleRoot), "Single-pack directory is incorrect.");

        const auto multiRoot = temporary.path / "multi";
        writeFile(
            multiRoot / "direct-bp" / "manifest.json",
            basicManifest(
                "Direct Behavior",
                "data",
                "03030303-0303-4303-8303-030303030303",
                "04040404-0404-4404-8404-040404040404"
            )
        );
        writeFile(
            multiRoot / "direct-rp" / "manifest.json",
            basicManifest(
                "Direct Resources",
                "resources",
                "05050505-0505-4505-8505-050505050505",
                "06060606-0606-4606-8606-060606060606"
            )
        );
        writeFile(
            multiRoot / "nested-configured" / "configured-pack" / "manifest.json",
            basicManifest(
                "Configured Pack",
                "data",
                "07070707-0707-4707-8707-070707070707",
                "08080808-0808-4808-8808-080808080808"
            )
        );

        const auto externalRoot = temporary.path / "external";
        const auto externalPack = externalRoot / "external-pack";
        writeFile(
            externalPack / "manifest.json",
            basicManifest(
                "External Pack",
                "data",
                "09090909-0909-4909-8909-090909090909",
                "10101010-1010-4010-8010-101010101010"
            )
        );
        const auto externalWorld = externalRoot / "external-world";
        fs::create_directories(externalWorld / "db");

        const auto      linkedPack = multiRoot / "linked-external-pack";
        std::error_code linkError;
        fs::create_directory_symlink(externalPack, linkedPack, linkError);

        std::string config = "{\n"
                             "  \"included_mod_dirs\": [\"./nested-configured\", \""
                           + pathToUtf8(externalPack) + "\"";
        if (!linkError) {
            config += ", \"./linked-external-pack\"";
        }
        config += "],\n  \"world_source_path\": \"" + pathToUtf8(externalWorld) + "\"\n}\n";
        writeFile(multiRoot / ".mcdev.json", config);

        const auto multi = mcdk::project::inspectProject(multiRoot);
        require(
            multi.kind == mcdk::project::ProjectKind::Addon,
            "Direct multi-pack workspace was not identified as an AddOn."
        );
        require(multi.manifests.size() == 3, "Configured in-workspace pack discovery is incorrect.");
        require(multi.behaviorPackCount() == 2, "Multi-pack behavior count is incorrect.");
        require(multi.resourcePackCount() == 1, "Multi-pack resource count is incorrect.");
        require(
            manifestNamed(multi, "Configured Pack").packDirectory.parent_path().filename() == "nested-configured",
            "Workspace-contained configured directory was not scanned."
        );
        require(multi.worldDirectory == std::nullopt, "External world source must not become the project map root.");
        require(multi.warnings.size() >= 2, "External configured paths did not produce warnings.");
        for (const auto& manifest : multi.manifests) {
            require(manifest.name != "External Pack", "External or linked pack was followed into discovery.");
            require(
                manifest.path.lexically_normal().string().starts_with(multiRoot.lexically_normal().string()),
                "Discovered manifest escaped the workspace."
            );
        }
        if (!linkError) {
            require(
                std::ranges::any_of(
                    multi.warnings,
                    [](const auto& warning) { return warning.find("linked") != std::string::npos; }
                ),
                "Linked directory was skipped without a warning."
            );
        }

        const auto externalBefore = readFile(externalPack / "manifest.json");
        const auto regenerated    = mcdk::project::regenerateProjectUuids(multiRoot);
        require(regenerated.project.manifests.size() == 3, "Mutation rediscovery crossed the workspace boundary.");
        require(
            readFile(externalPack / "manifest.json") == externalBefore,
            "UUID refresh modified an external or linked configured pack."
        );
    }

    void testImplicitRootScanIgnoresDevelopmentOutputsUnlessExplicitlyIncluded() {
        TemporaryDirectory temporary;
        const auto         root          = temporary.path / "ignored-development-output";
        const auto         primaryPath   = root / "pack" / "manifest.json";
        const auto         buildManifest = root / "BuIlD" / "manifest.json";

        constexpr std::string_view primaryHeaderUuid = "29292929-2929-4929-8929-292929292929";
        writeFile(
            primaryPath,
            basicManifest("Primary Pack", "data", primaryHeaderUuid, "30303030-3030-4030-8030-303030303030")
        );
        writeFile(
            buildManifest,
            basicManifest("Build Copy", "data", primaryHeaderUuid, "31313131-3131-4131-8131-313131313131")
        );
        writeFile(root / ".mcdev.json", R"({"included_mod_dirs":["./"]})");

        auto summary = mcdk::project::inspectProject(root);
        require(summary.manifests.size() == 1, "An included_mod_dirs root alias bypassed implicit build filtering.");
        require(summary.manifests.front().name == "Primary Pack", "Implicit build filtering hid the real pack.");

        writeFile(
            buildManifest,
            basicManifest(
                "Explicit Build Pack",
                "resources",
                "32323232-3232-4232-8232-323232323232",
                "33333333-3333-4333-8333-333333333333"
            )
        );
        writeFile(root / ".mcdev.json", R"({"included_mod_dirs":[{"path":"./BuIlD","enabled":true}]})");

        summary = mcdk::project::inspectProject(root);
        require(summary.manifests.size() == 2, "An explicit included build directory did not override filtering.");
        require(
            manifestNamed(summary, "Explicit Build Pack").path == fs::canonical(buildManifest),
            "Explicit build directory discovery selected the wrong manifest."
        );
    }

    void testAllVersionPartsUpdateHeaderAndModules() {
        TemporaryDirectory temporary;
        const auto         root         = temporary.path / "version-parts";
        const auto         manifestPath = root / "manifest.json";
        writeFile(
            manifestPath,
            basicManifest(
                "Version Parts",
                "data",
                "12121212-1212-4212-8212-121212121212",
                "13131313-1313-4313-8313-131313131313"
            )
        );

        auto result = mcdk::project::bumpProjectVersion(root, mcdk::project::VersionPart::Patch);
        require(
            result.project.manifests.front().version == mcdk::project::Version{1, 2, 4},
            "Patch bump semantics are incorrect."
        );
        require(
            readFile(manifestPath).find("[1, 2, 4]") != std::string::npos,
            "Patch bump did not update serialized versions."
        );

        result = mcdk::project::bumpProjectVersion(root, mcdk::project::VersionPart::Minor);
        require(
            result.project.manifests.front().version == mcdk::project::Version{1, 3, 0},
            "Minor bump semantics are incorrect."
        );
        const auto afterMinor = readFile(manifestPath);
        require(afterMinor.find("[1, 3, 0]") != std::string::npos, "Minor bump was not serialized.");
        require(afterMinor.find("[1, 2, 4]") == std::string::npos, "Module version was not reset by minor bump.");

        result = mcdk::project::bumpProjectVersion(root, mcdk::project::VersionPart::Major);
        require(
            result.project.manifests.front().version == mcdk::project::Version{2, 0, 0},
            "Major bump semantics are incorrect."
        );
        const auto afterMajor = readFile(manifestPath);
        const auto first      = afterMajor.find("[2, 0, 0]");
        require(
            first != std::string::npos && afterMajor.find("[2, 0, 0]", first + 1) != std::string::npos,
            "Major bump did not update both header and module versions."
        );
    }

    void testWorldSourceTriStateControlsAutomaticMapDetection() {
        TemporaryDirectory temporary;
        const auto         root          = temporary.path / "world-source-state";
        const auto         manifestPath  = root / "pack" / "manifest.json";
        const auto         worldListPath = root / "world_behavior_packs.json";
        fs::create_directories(root / "db");
        writeFile(
            manifestPath,
            basicManifest(
                "Pack",
                "data",
                "23232323-2323-4323-8323-232323232323",
                "24242424-2424-4424-8424-242424242424"
            )
        );
        writeFile(
            worldListPath,
            R"([{"pack_id":"23232323-2323-4323-8323-232323232323","version":[1,2,3],"type":"Addon"}])"
        );
        const auto manifestBefore  = readFile(manifestPath);
        const auto worldListBefore = readFile(worldListPath);

        writeFile(root / ".mcdev.json", R"({"world_source_path": null})");
        auto summary = mcdk::project::inspectProject(root);
        require(
            summary.kind == mcdk::project::ProjectKind::SinglePack,
            "null world source did not disable map auto detection."
        );
        require(!summary.worldDirectory.has_value(), "null world source unexpectedly selected the workspace as a map.");
        require(
            summary.packDirectories.front() == fs::canonical(root / "pack"),
            "Disabled map source changed the clean pack root."
        );
        require(summary.worldPackListFiles.empty(), "null world source still discovered root world pack lists.");
        auto operation = mcdk::project::bumpProjectVersion(root, mcdk::project::VersionPart::Patch);
        require(readFile(worldListPath) == worldListBefore, "null world source allowed a root world list mutation.");
        require(
            std::ranges::none_of(
                operation.modifiedFiles,
                [&](const auto& path) { return path.lexically_normal() == worldListPath.lexically_normal(); }
            ),
            "null world source reported a root world list as modified."
        );
        writeFile(manifestPath, manifestBefore);

        writeFile(root / ".mcdev.json", R"({"world_source_path": ""})");
        summary = mcdk::project::inspectProject(root);
        require(
            summary.kind == mcdk::project::ProjectKind::SinglePack,
            "Empty world source did not disable map auto detection."
        );
        require(
            !summary.worldDirectory.has_value(),
            "Empty world source unexpectedly selected the workspace as a map."
        );
        require(summary.worldPackListFiles.empty(), "Empty world source still discovered root world pack lists.");
        operation = mcdk::project::regenerateProjectUuids(root);
        require(readFile(worldListPath) == worldListBefore, "Empty world source allowed a root world list mutation.");
        require(
            std::ranges::none_of(
                operation.modifiedFiles,
                [&](const auto& path) { return path.lexically_normal() == worldListPath.lexically_normal(); }
            ),
            "Empty world source reported a root world list as modified."
        );
        writeFile(manifestPath, manifestBefore);

        writeFile(root / ".mcdev.json", R"({"world_source_path": "./missing-world"})");
        summary = mcdk::project::inspectProject(root);
        require(
            summary.kind == mcdk::project::ProjectKind::SinglePack && !summary.worldDirectory,
            "A missing explicit world source fell back to the workspace map marker."
        );
        require(summary.worldPackListFiles.empty(), "A missing explicit world source discovered root world lists.");
        require(!summary.warnings.empty(), "A missing explicit world source did not produce a warning.");
        operation = mcdk::project::bumpProjectVersion(root, mcdk::project::VersionPart::Patch);
        require(
            readFile(worldListPath) == worldListBefore,
            "A missing explicit world source modified a root world list."
        );
        require(
            std::ranges::none_of(
                operation.modifiedFiles,
                [&](const auto& path) { return path.lexically_normal() == worldListPath.lexically_normal(); }
            ),
            "A missing explicit world source reported a root world list as modified."
        );
        writeFile(manifestPath, manifestBefore);

        const auto externalWorld = temporary.path / "external-world";
        fs::create_directories(externalWorld / "db");
        writeFile(root / ".mcdev.json", "{\"world_source_path\":\"" + pathToUtf8(externalWorld) + "\"}");
        summary = mcdk::project::inspectProject(root);
        require(
            summary.kind == mcdk::project::ProjectKind::SinglePack && !summary.worldDirectory,
            "An external explicit world source fell back to the workspace map marker."
        );
        require(summary.worldPackListFiles.empty(), "An external explicit world source discovered root world lists.");
        require(!summary.warnings.empty(), "An external explicit world source did not produce a warning.");
        operation = mcdk::project::regenerateProjectUuids(root);
        require(
            readFile(worldListPath) == worldListBefore,
            "An external explicit world source modified a root world list."
        );
        require(
            std::ranges::none_of(
                operation.modifiedFiles,
                [&](const auto& path) { return path.lexically_normal() == worldListPath.lexically_normal(); }
            ),
            "An external explicit world source reported a root world list as modified."
        );
        writeFile(manifestPath, manifestBefore);

        writeFile(root / ".mcdev.json", R"({"world_source_path": "auto"})");
        summary = mcdk::project::inspectProject(root);
        require(
            summary.kind == mcdk::project::ProjectKind::Map,
            "auto world source did not detect the root map marker."
        );
        require(summary.worldDirectory == fs::canonical(root), "auto world source selected the wrong map root.");
        require(summary.worldPackListFiles.size() == 1, "auto world source did not discover the root world pack list.");

        const auto markerOnlyRoot = temporary.path / "marker-only-explicit-world";
        const auto markerOnlyList = markerOnlyRoot / "world_behavior_packs.json";
        fs::create_directories(markerOnlyRoot / "db");
        writeFile(markerOnlyList, "[]");
        writeFile(markerOnlyRoot / ".mcdev.json", R"({"world_source_path":"./missing-world"})");
        const auto markerOnlyBefore = readFile(markerOnlyList);
        bool       invalidProject   = false;
        try {
            (void)mcdk::project::inspectProject(markerOnlyRoot);
        } catch (const mcdk::project::ProjectError& error) {
            invalidProject = error.code() == mcdk::project::ProjectErrorCode::InvalidProject;
        }
        require(invalidProject, "A missing explicit world source without manifests was accepted as a map.");
        require(readFile(markerOnlyList) == markerOnlyBefore, "Failed explicit world discovery modified a root list.");
    }

    void testMapMutationIsLosslessAndSynchronized() {
        TemporaryDirectory temporary;
        const auto         root = temporary.path / fs::u8path("项目配置测试");
        fs::create_directories(root / "db");

        constexpr std::string_view behaviorUuid     = "11111111-1111-4111-8111-111111111111";
        constexpr std::string_view resourceUuid     = "22222222-2222-4222-8222-222222222222";
        const auto                 behaviorManifest = root / "behavior_packs" / fs::u8path("行为包") / "manifest.json";
        const auto                 resourceManifest = root / "resource_packs" / fs::u8path("资源包") / "manifest.json";
        writeFile(behaviorManifest, std::string("\xef\xbb\xbf") + R"({
    // header comment must survive
    "format_version": 2,
    "header": {
        "name": "Behavior",
        "uuid": "11111111-1111-4111-8111-111111111111",
        "version": [1, 2, 3,],
    },
    "modules": [{
        "type": "data",
        "uuid": "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
        "version": [1, 2, 3,],
    }],
    "dependencies": [{
        "uuid": "22222222-2222-4222-8222-222222222222",
        "version": [0, 0, 1,],
    }],
})");
        writeFile(
            resourceManifest,
            R"({
    /* resource comment must survive */
    "format_version": 2,
    "header": {
        "name": "Resources",
        "uuid": "22222222-2222-4222-8222-222222222222",
        "version": [4, 5, 6,],
    },
    "modules": [{
        "type": "resources",
        "uuid": "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb",
        "version": [4, 5, 6,],
    }],
})"
        );

        writeFile(
            root / ".mcdev.json",
            R"({
                "included_mod_dirs": ["./behavior_packs", "./resource_packs", "C:/outside-workspace"],
                "world_source_path": "./",
            })"
        );

        for (const auto name : {
                 "world_behavior_packs.json",
                 "netease_world_behavior_packs.json",
             }) {
            writeFile(
                root / name,
                R"([
                    // world comment must survive
                    {"pack_id": "11111111-1111-4111-8111-111111111111", "version": [1, 2, 3,], "type": "Addon"},
                ])"
            );
        }
        for (const auto name : {
                 "world_resource_packs.json",
                 "netease_world_resource_packs.json",
             }) {
            writeFile(
                root / name,
                R"([
                    {"pack_id": "22222222-2222-4222-8222-222222222222", "version": [4, 5, 6,], "type": "Addon"},
                ])"
            );
        }

        const auto inspected = mcdk::project::inspectProject(root);
        require(inspected.kind == mcdk::project::ProjectKind::Map, "Gameplay map was not identified.");
        require(inspected.behaviorPackCount() == 1, "Behavior pack count is incorrect.");
        require(inspected.resourcePackCount() == 1, "Resource pack count is incorrect.");
        require(inspected.worldDirectory.has_value(), "Map content directory was not reported.");
        require(inspected.worldPackListFiles.size() == 4, "All standard and NetEase world lists must be discovered.");
        require(!inspected.warnings.empty(), "External configured path should produce a warning.");

        const auto regenerated = mcdk::project::regenerateProjectUuids(root);
        require(regenerated.modifiedFiles.size() == 6, "UUID refresh did not update all pack and world files.");
        const auto newBehaviorUuid = manifestNamed(regenerated.project, "Behavior").uuid;
        const auto newResourceUuid = manifestNamed(regenerated.project, "Resources").uuid;
        require(newBehaviorUuid != behaviorUuid, "Behavior header UUID did not change.");
        require(newResourceUuid != resourceUuid, "Resource header UUID did not change.");
        require(newBehaviorUuid[14] == '4', "Generated UUID is not RFC 4122 version 4.");
        require(
            newBehaviorUuid[19] == '8' || newBehaviorUuid[19] == '9' || newBehaviorUuid[19] == 'a'
                || newBehaviorUuid[19] == 'b',
            "Generated UUID does not use the RFC 4122 variant."
        );

        const auto behaviorAfterUuid = readFile(behaviorManifest);
        require(behaviorAfterUuid.starts_with("\xef\xbb\xbf"), "UTF-8 BOM was not preserved.");
        require(behaviorAfterUuid.contains("// header comment must survive"), "Line comment was not preserved.");
        require(behaviorAfterUuid.contains("[1, 2, 3,]"), "Trailing comma or version formatting was lost.");
        require(behaviorAfterUuid.contains(newResourceUuid), "Internal dependency UUID was not synchronized.");
        require(!behaviorAfterUuid.contains(resourceUuid), "Old dependency UUID remains after refresh.");

        for (const auto name : {
                 "world_behavior_packs.json",
                 "netease_world_behavior_packs.json",
             }) {
            const auto content = readFile(root / name);
            require(content.contains(newBehaviorUuid), "World behavior pack_id was not synchronized.");
            require(content.contains("\"type\": \"Addon\""), "NetEase type field was not preserved.");
            require(content.contains("// world comment must survive"), "World JSONC comment was not preserved.");
        }

        const auto behaviorBeforeRollback = readFile(behaviorManifest);
        const auto resourceBeforeRollback = readFile(resourceManifest);
        setCommitFailure("1");
        bool rollbackFailedAsExpected = false;
        try {
            (void)mcdk::project::bumpProjectVersion(root, mcdk::project::VersionPart::Patch);
        } catch (const mcdk::project::ProjectError& error) {
            rollbackFailedAsExpected = error.code() == mcdk::project::ProjectErrorCode::IoError;
        }
        setCommitFailure("");
        require(rollbackFailedAsExpected, "Injected commit failure did not surface as an I/O error.");
        require(readFile(behaviorManifest) == behaviorBeforeRollback, "Behavior manifest rollback failed.");
        require(readFile(resourceManifest) == resourceBeforeRollback, "Resource manifest rollback failed.");
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            require(
                !entry.path().filename().string().starts_with(".mcdk-project-"),
                "Transaction left a temporary file."
            );
        }

        const auto bumped = mcdk::project::bumpProjectVersion(root, mcdk::project::VersionPart::Patch);
        require(
            manifestNamed(bumped.project, "Behavior").version == mcdk::project::Version{1, 2, 4},
            "Behavior patch version is incorrect."
        );
        require(
            manifestNamed(bumped.project, "Resources").version == mcdk::project::Version{4, 5, 7},
            "Resource patch version is incorrect."
        );
        const auto behaviorAfterVersion = readFile(behaviorManifest);
        require(
            behaviorAfterVersion.contains("\"version\": [4, 5, 7,]"),
            "Dependency version did not use the referenced pack version."
        );
        require(behaviorAfterVersion.contains("// header comment must survive"), "Version bump lost comments.");
        require(
            readFile(root / "world_resource_packs.json").contains("[4, 5, 7,]"),
            "World resource pack version was not synchronized."
        );
    }

    void testDuplicateHeaderUuidFailsBeforeWrites() {
        TemporaryDirectory temporary;
        const auto         root     = temporary.path / "duplicate";
        const auto         manifest = R"({
            "format_version": 2,
            "header": {"name": "Pack", "uuid": "33333333-3333-4333-8333-333333333333", "version": [1, 0, 0]},
            "modules": [{"type": "data", "uuid": "44444444-4444-4444-8444-444444444444", "version": [1, 0, 0]}]
        })";
        writeFile(root / "one" / "manifest.json", manifest);
        writeFile(root / "two" / "manifest.json", manifest);
        const auto before = readFile(root / "one" / "manifest.json");

        bool rejected = false;
        try {
            (void)mcdk::project::regenerateProjectUuids(root);
        } catch (const mcdk::project::ProjectError& error) {
            rejected = error.code() == mcdk::project::ProjectErrorCode::DuplicateUuid;
        }
        require(rejected, "Duplicate header UUIDs were not rejected.");
        require(readFile(root / "one" / "manifest.json") == before, "Preflight failure changed a manifest.");
    }

    void testVersionOverflowFailsBeforeWrites() {
        TemporaryDirectory temporary;
        const auto         root = temporary.path / "overflow";
        const auto         path = root / "manifest.json";
        writeFile(
            path,
            R"({
                "format_version": 2,
                "header": {"name": "Overflow", "uuid": "55555555-5555-4555-8555-555555555555", "version": [4294967295, 1, 2]},
                "modules": [{"type": "data", "uuid": "66666666-6666-4666-8666-666666666666", "version": [4294967295, 1, 2]}]
            })"
        );
        const auto before   = readFile(path);
        bool       rejected = false;
        try {
            (void)mcdk::project::bumpProjectVersion(root, mcdk::project::VersionPart::Major);
        } catch (const mcdk::project::ProjectError& error) {
            rejected = error.code() == mcdk::project::ProjectErrorCode::VersionOverflow;
        }
        require(rejected, "Overflowing version bump was not rejected.");
        require(readFile(path) == before, "Overflow preflight failure changed the manifest.");
    }

    void testMalformedAndMissingFieldsFailBeforeAnyWrite() {
        TemporaryDirectory temporary;
        const auto         root        = temporary.path / "invalid-preflight";
        const auto         validPath   = root / "a-valid" / "manifest.json";
        const auto         invalidPath = root / "b-invalid" / "manifest.json";
        writeFile(
            validPath,
            basicManifest(
                "Valid",
                "data",
                "14141414-1414-4414-8414-141414141414",
                "15151515-1515-4515-8515-151515151515"
            )
        );
        writeFile(invalidPath, R"({"header": { /* unterminated comment )");
        const auto validBefore   = readFile(validPath);
        const auto invalidBefore = readFile(invalidPath);

        bool malformedRejected = false;
        try {
            (void)mcdk::project::regenerateProjectUuids(root);
        } catch (const mcdk::project::ProjectError& error) {
            malformedRejected = error.code() == mcdk::project::ProjectErrorCode::InvalidManifest;
        }
        require(malformedRejected, "Malformed JSONC was not rejected during preflight.");
        require(readFile(validPath) == validBefore, "Malformed peer manifest caused a partial write.");
        require(readFile(invalidPath) == invalidBefore, "Malformed manifest changed during failed preflight.");

        writeFile(
            invalidPath,
            R"({
                "format_version": 2,
                "header": {"name": "Missing Module UUID", "uuid": "16161616-1616-4616-8616-161616161616", "version": [1, 0, 0]},
                "modules": [{"type": "data", "version": [1, 0, 0]}]
            })"
        );
        const auto missingBefore   = readFile(invalidPath);
        bool       missingRejected = false;
        try {
            (void)mcdk::project::bumpProjectVersion(root, mcdk::project::VersionPart::Patch);
        } catch (const mcdk::project::ProjectError& error) {
            missingRejected = error.code() == mcdk::project::ProjectErrorCode::MissingField;
        }
        require(missingRejected, "Missing required manifest field was not rejected during preflight.");
        require(readFile(validPath) == validBefore, "Missing field caused a partial write to another manifest.");
        require(readFile(invalidPath) == missingBefore, "Missing-field manifest changed during failed preflight.");
    }

    void testOccupiedBackupPathIsNeverReplacedOrDeleted() {
        TemporaryDirectory temporary;
        const auto         root         = temporary.path / "occupied-backup";
        const auto         manifestPath = root / "manifest.json";
        writeFile(
            manifestPath,
            basicManifest(
                "Occupied Backup",
                "data",
                "44444444-4444-4444-8444-444444444444",
                "45454545-4545-4545-8545-454545454545"
            )
        );
        const auto before = readFile(manifestPath);

        setOccupiedBackupInjection("1");
        bool sourceChanged = false;
        try {
            (void)mcdk::project::bumpProjectVersion(root, mcdk::project::VersionPart::Patch);
        } catch (const mcdk::project::ProjectError& error) {
            sourceChanged = error.code() == mcdk::project::ProjectErrorCode::SourceChanged;
        } catch (...) {
            setOccupiedBackupInjection("");
            throw;
        }
        setOccupiedBackupInjection("");

        require(sourceChanged, "An occupied backup destination was not reported as source_changed.");
        require(readFile(manifestPath) == before, "An occupied backup destination changed the source manifest.");

        std::vector<fs::path> transactionPaths;
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            if (entry.path().filename().string().starts_with(".mcdk-project-")) {
                transactionPaths.push_back(entry.path());
            }
        }
        require(transactionPaths.size() == 1, "The occupied-backup conflict left an unexpected transaction path.");
        require(
            transactionPaths.front().filename().string().ends_with(".rollback.tmp"),
            "The preserved path is not the injected backup occupant."
        );
        require(
            readFile(transactionPaths.front()) == "external backup-path occupant\n",
            "The no-replace claim overwrote or deleted the external backup-path occupant."
        );
        fs::remove(transactionPaths.front());
    }

#ifdef _WIN32
    void testPreexistingWindowsWriterPreventsAnyTransaction() {
        TemporaryDirectory temporary;
        const auto         root       = temporary.path / "preexisting-writer";
        const auto         firstPath  = root / "a-pack" / "manifest.json";
        const auto         secondPath = root / "b-pack" / "manifest.json";
        writeFile(
            firstPath,
            basicManifest(
                "Open Writer",
                "data",
                "34343434-3434-4434-8434-343434343434",
                "35353535-3535-4535-8535-353535353535"
            )
        );
        writeFile(
            secondPath,
            basicManifest(
                "Unchanged Peer",
                "resources",
                "36363636-3636-4636-8636-363636363636",
                "37373737-3737-4737-8737-373737373737"
            )
        );
        const auto                 firstBefore  = readFile(firstPath);
        const auto                 secondBefore = readFile(secondPath);
        constexpr std::string_view marker       = "\n// append from preexisting writer\n";

        {
            HeldAppendHandle writer(firstPath);
            bool             sourceChanged = false;
            try {
                (void)mcdk::project::bumpProjectVersion(root, mcdk::project::VersionPart::Patch);
            } catch (const mcdk::project::ProjectError& error) {
                sourceChanged = error.code() == mcdk::project::ProjectErrorCode::SourceChanged;
            }
            require(sourceChanged, "A preexisting Windows writer did not block the project transaction.");
            require(readFile(firstPath) == firstBefore, "The guarded manifest changed before the writer appended.");
            require(readFile(secondPath) == secondBefore, "A writer conflict modified another manifest.");
            for (const auto& entry : fs::recursive_directory_iterator(root)) {
                require(
                    !entry.path().filename().string().starts_with(".mcdk-project-"),
                    "A writer conflict left a transaction file behind."
                );
            }
            writer.append(marker);
        }

        require(
            readFile(firstPath) == firstBefore + std::string(marker),
            "Content appended through the preexisting handle was lost after the failed transaction."
        );
        require(readFile(secondPath) == secondBefore, "The peer manifest changed after the writer closed.");
    }

    void testPreexistingWindowsReaderWithoutDeleteSharePreventsAnyTransaction() {
        TemporaryDirectory temporary;
        const auto         root       = temporary.path / "preexisting-reader";
        const auto         firstPath  = root / "a-pack" / "manifest.json";
        const auto         secondPath = root / "b-pack" / "manifest.json";
        writeFile(
            firstPath,
            basicManifest(
                "Open Reader",
                "data",
                "40404040-4040-4040-8040-404040404040",
                "41414141-4141-4141-8141-414141414141"
            )
        );
        writeFile(
            secondPath,
            basicManifest(
                "Reader Peer",
                "resources",
                "42424242-4242-4242-8242-424242424242",
                "43434343-4343-4343-8343-434343434343"
            )
        );
        const auto firstBefore  = readFile(firstPath);
        const auto secondBefore = readFile(secondPath);

        {
            HeldNoDeleteShareReadHandle reader(firstPath);
            bool                        sourceChanged = false;
            try {
                (void)mcdk::project::regenerateProjectUuids(root);
            } catch (const mcdk::project::ProjectError& error) {
                sourceChanged = error.code() == mcdk::project::ProjectErrorCode::SourceChanged;
            }
            require(sourceChanged, "A no-delete-share reader did not block the project transaction.");
            require(readFile(firstPath) == firstBefore, "A reader conflict modified the guarded manifest.");
            require(readFile(secondPath) == secondBefore, "A reader conflict modified another manifest.");
        }
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            require(
                !entry.path().filename().string().starts_with(".mcdk-project-"),
                "A reader conflict left a transaction file behind."
            );
        }
    }

    void testProtectedTransactionPathsRejectNoDeleteShareReaders() {
        TemporaryDirectory temporary;
        const auto         root         = temporary.path / "protected-transaction-paths";
        const auto         manifestPath = root / "manifest.json";
        writeFile(
            manifestPath,
            basicManifest(
                "Protected Paths",
                "data",
                "38383838-3838-4838-8838-383838383838",
                "39393939-3939-4939-8939-393939393939"
            )
        );

        setFileGuardProbe("1");
        mcdk::project::OperationResult operation;
        try {
            operation = mcdk::project::bumpProjectVersion(root, mcdk::project::VersionPart::Patch);
        } catch (...) {
            setFileGuardProbe("");
            throw;
        }
        setFileGuardProbe("");

        require(
            operation.project.manifests.front().version == mcdk::project::Version{1, 2, 4},
            "File guard probes prevented a valid protected transaction."
        );
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            require(
                !entry.path().filename().string().starts_with(".mcdk-project-"),
                "A successful protected transaction left a temporary hardlink or backup."
            );
        }
    }
#endif

    void testExternalSaveBeforeCommitIsNeverOverwritten() {
        TemporaryDirectory temporary;
        const auto         root       = temporary.path / "source-changed";
        const auto         firstPath  = root / "a-pack" / "manifest.json";
        const auto         secondPath = root / "b-pack" / "manifest.json";
        writeFile(
            firstPath,
            basicManifest(
                "First",
                "data",
                "19191919-1919-4919-8919-191919191919",
                "20202020-2020-4020-8020-202020202020"
            )
        );
        writeFile(
            secondPath,
            basicManifest(
                "Second",
                "resources",
                "21212121-2121-4121-8121-212121212121",
                "22222222-2222-4222-8222-222222222222"
            )
        );
        const auto firstBefore  = readFile(firstPath);
        const auto secondBefore = readFile(secondPath);

        setSourceMutation("1");
        bool sourceChanged = false;
        try {
            (void)mcdk::project::bumpProjectVersion(root, mcdk::project::VersionPart::Patch);
        } catch (const mcdk::project::ProjectError& error) {
            sourceChanged =
                error.code() == mcdk::project::ProjectErrorCode::SourceChanged && error.codeName() == "source_changed";
        }
        setSourceMutation("");

        require(sourceChanged, "An external save before commit was not reported as source_changed.");
        require(
            readFile(firstPath) == firstBefore + "\n// simulated external save before project commit\n",
            "The external save was overwritten or altered by the failed project operation."
        );
        require(readFile(secondPath) == secondBefore, "Source conflict caused a partial write to another manifest.");
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            require(
                !entry.path().filename().string().starts_with(".mcdk-project-"),
                "Source conflict left a transaction file behind."
            );
        }
    }

    void testLaterExternalSaveAfterFirstClaimIsPreserved() {
        TemporaryDirectory temporary;
        const auto         root       = temporary.path / "source-changed-during-claim";
        const auto         firstPath  = root / "a-pack" / "manifest.json";
        const auto         secondPath = root / "b-pack" / "manifest.json";
        writeFile(
            firstPath,
            basicManifest(
                "First Claimed",
                "data",
                "25252525-2525-4525-8525-252525252525",
                "26262626-2626-4626-8626-262626262626"
            )
        );
        writeFile(
            secondPath,
            basicManifest(
                "Second Edited",
                "resources",
                "27272727-2727-4727-8727-272727272727",
                "28282828-2828-4828-8828-282828282828"
            )
        );
        const auto firstBefore  = readFile(firstPath);
        const auto secondBefore = readFile(secondPath);

        setSourceMutationAfterFirstClaim("2");
        bool sourceChanged = false;
        try {
            (void)mcdk::project::regenerateProjectUuids(root);
        } catch (const mcdk::project::ProjectError& error) {
            sourceChanged = error.code() == mcdk::project::ProjectErrorCode::SourceChanged;
        }
        setSourceMutationAfterFirstClaim("");

        require(sourceChanged, "A later external save during source claiming was not rejected.");
        require(readFile(firstPath) == firstBefore, "The already-claimed first manifest was not restored.");
        require(
            readFile(secondPath) == secondBefore + "\n// simulated external save after first source claim\n",
            "The later external edit was lost during rollback."
        );
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            require(
                !entry.path().filename().string().starts_with(".mcdk-project-"),
                "Claim conflict left a transaction or backup file behind."
            );
        }
    }

    void testExternalTargetPreviewSynchronizesWorkspaceAndRollsBackAtomically() {
        TemporaryDirectory temporary;
        const auto         workspace        = temporary.path / "target-aware-workspace";
        const auto         externalTarget   = temporary.path / "disabled-external-target";
        const auto         targetManifest   = externalTarget / "manifest.json";
        const auto         consumerManifest = workspace / "consumer" / "manifest.json";
        const std::array   worldLists{
            workspace / "world_behavior_packs.json",
            workspace / "world_resource_packs.json",
            workspace / "netease_world_behavior_packs.json",
            workspace / "netease_world_resource_packs.json",
        };
        constexpr std::string_view targetUuid = "41414141-4141-4141-8141-414141414141";

        writeFile(
            targetManifest,
            basicManifest("External Target", "data", targetUuid, "42424242-4242-4242-8242-424242424242", "[1, 0, 0]")
        );
        writeFile(
            consumerManifest,
            manifestWithDependency(
                "Workspace Consumer",
                "43434343-4343-4343-8343-434343434343",
                "44444444-4444-4444-8444-444444444444",
                targetUuid
            )
        );
        writeFile(workspace / "level.dat", "level");
        for (const auto& worldList : worldLists) {
            writeFile(worldList, "[{\"pack_id\": \"" + std::string(targetUuid) + "\", \"version\": [1, 0, 0]}]\n");
        }
        writeFile(
            workspace / ".mcdev.json",
            "{\"included_mod_dirs\":[{\"path\":\"" + pathToUtf8(externalTarget) + "\",\"enabled\":false}]}\n"
        );

        const auto               targetBefore   = readFile(targetManifest);
        const auto               consumerBefore = readFile(consumerManifest);
        std::vector<std::string> worldBefore;
        for (const auto& worldList : worldLists) {
            worldBefore.push_back(readFile(worldList));
        }
        const auto prepared =
            mcdk::project::bumpProjectVersion(workspace, mcdk::project::VersionPart::Patch, targetManifest, true);
        require(prepared.preview.has_value(), "Target-aware version preview was not returned.");
        require(prepared.modifiedFiles.empty(), "Preview mode reported committed files.");
        require(prepared.preview->target == fs::canonical(externalTarget), "Manifest target was not normalized.");
        require(prepared.preview->files.size() == 6, "Target preview did not include all synchronized references.");
        require(prepared.project.root == fs::canonical(externalTarget), "Target summary kept the workspace root.");
        require(prepared.project.name == externalTarget.filename().string(), "Target summary kept the workspace name.");
        require(prepared.project.manifests.size() == 1, "Target summary included workspace reference manifests.");
        require(
            prepared.project.manifests.front().path == fs::canonical(targetManifest),
            "Target summary did not report the selected manifest."
        );
        require(readFile(targetManifest) == targetBefore, "Preview changed the external target.");
        require(readFile(consumerManifest) == consumerBefore, "Preview changed a workspace dependency.");
        for (std::size_t index = 0; index < worldLists.size(); ++index) {
            require(readFile(worldLists[index]) == worldBefore[index], "Preview changed a world pack list.");
        }

        mcdk::project::MutationPreview reconstructed;
        reconstructed.id             = prepared.preview->id;
        reconstructed.operation      = prepared.preview->operation;
        reconstructed.root           = prepared.preview->root;
        reconstructed.target         = prepared.preview->target;
        reconstructed.versionPart    = prepared.preview->versionPart;
        reconstructed.files          = prepared.preview->files;
        reconstructed.opaqueApproval = prepared.preview->opaqueApproval;

        setCommitFailure("1");
        bool rolledBack = false;
        try {
            (void)mcdk::project::applyProjectPreview(workspace, reconstructed);
        } catch (const mcdk::project::ProjectError& error) {
            rolledBack = error.code() == mcdk::project::ProjectErrorCode::IoError;
        }
        setCommitFailure("");
        require(rolledBack, "Injected cross-root preview commit failure did not surface.");
        require(readFile(targetManifest) == targetBefore, "Cross-root rollback did not restore the target.");
        require(readFile(consumerManifest) == consumerBefore, "Cross-root rollback did not restore the dependency.");
        for (std::size_t index = 0; index < worldLists.size(); ++index) {
            require(
                readFile(worldLists[index]) == worldBefore[index],
                "Cross-root rollback did not restore a world list."
            );
        }

        const auto applied = mcdk::project::applyProjectPreview(workspace, reconstructed);
        require(applied.modifiedFiles.size() == 6, "Applying a target preview returned the wrong file count.");
        require(readFile(targetManifest).find("[1, 0, 1]") != std::string::npos, "Target version was not bumped.");
        require(
            readFile(consumerManifest).find("[1, 0, 1]") != std::string::npos,
            "Workspace dependency version was not synchronized."
        );
        for (const auto& worldList : worldLists) {
            require(
                readFile(worldList).find("[1, 0, 1]") != std::string::npos,
                "World pack list version was not synchronized."
            );
        }

        bool stale = false;
        try {
            (void)mcdk::project::applyProjectPreview(workspace, reconstructed);
        } catch (const mcdk::project::ProjectError& error) {
            stale = error.code() == mcdk::project::ProjectErrorCode::PreviewStale;
        }
        require(stale, "Reusing an applied preview was not reported as preview_stale.");
    }

    void testReadOnlyExternalReferenceBlocksTargetMutation() {
        TemporaryDirectory         temporary;
        const auto                 workspace        = temporary.path / "out-of-scope-workspace";
        const auto                 targetManifest   = workspace / "target" / "manifest.json";
        const auto                 externalManifest = temporary.path / "read-only-reference" / "manifest.json";
        constexpr std::string_view targetUuid       = "51515151-5151-4151-8151-515151515151";
        writeFile(
            targetManifest,
            basicManifest("Selected Target", "data", targetUuid, "52525252-5252-4252-8252-525252525252", "[1, 0, 0]")
        );
        writeFile(
            externalManifest,
            manifestWithDependency(
                "Read Only Reference",
                "53535353-5353-4353-8353-535353535353",
                "54545454-5454-4454-8454-545454545454",
                targetUuid
            )
        );
        writeFile(
            workspace / ".mcdev.json",
            "{\"included_mod_dirs\":[\"./target\",\"" + pathToUtf8(externalManifest.parent_path()) + "\"]}\n"
        );
        const auto workspaceInspection = mcdk::project::inspectProject(workspace);
        require(
            std::ranges::any_of(workspaceInspection.warnings, [](const auto& warning) {
                return warning.find("Ignored external included_mod_dirs") != std::string::npos;
            }),
            "Workspace inspection no longer reports an external configured Mod."
        );
        const auto targetInspection = mcdk::project::inspectProject(workspace, targetManifest);
        require(
            std::ranges::none_of(targetInspection.warnings, [](const auto& warning) {
                return warning.find("Ignored external included_mod_dirs") != std::string::npos;
            }),
            "Single-Mod inspection warned about another read-only external Mod."
        );
        const auto targetBefore   = readFile(targetManifest);
        const auto externalBefore = readFile(externalManifest);
        bool       blocked        = false;
        try {
            (void)mcdk::project::regenerateProjectUuids(workspace, targetManifest, false);
        } catch (const mcdk::project::ProjectError& error) {
            blocked = error.code() == mcdk::project::ProjectErrorCode::OutOfScopeReference
                   && error.codeName() == "out_of_scope_reference";
        }
        require(blocked, "A read-only external dependency was not blocked before mutation.");
        require(readFile(targetManifest) == targetBefore, "Blocked target mutation wrote the selected pack.");
        require(readFile(externalManifest) == externalBefore, "Blocked target mutation wrote an external reference.");
    }

    void testExternalWorldSourceReferencesAreReadOnly() {
        TemporaryDirectory         temporary;
        const auto                 workspace      = temporary.path / "external-world-reference-workspace";
        const auto                 targetManifest = workspace / "target" / "manifest.json";
        const auto                 externalWorld  = temporary.path / "external-world-source";
        const auto                 externalList   = externalWorld / "netease_world_resource_packs.json";
        constexpr std::string_view targetUuid     = "85858585-8585-4585-8585-858585858585";
        writeFile(
            targetManifest,
            basicManifest(
                "External World Target",
                "resources",
                targetUuid,
                "86868686-8686-4686-8686-868686868686",
                "[1, 0, 0]"
            )
        );
        writeFile(externalList, "[{\"pack_id\": \"" + std::string(targetUuid) + "\", \"version\": [1, 0, 0]}]\n");
        writeFile(workspace / ".mcdev.json", "{\"world_source_path\":\"" + pathToUtf8(externalWorld) + "\"}\n");

        const auto inspected = mcdk::project::inspectProject(workspace, targetManifest);
        require(!inspected.worldDirectory.has_value(), "External world source became the writable target world.");
        require(inspected.worldPackListFiles.empty(), "External world pack list leaked into the target summary.");
        require(
            std::ranges::any_of(
                inspected.warnings,
                [](const auto& warning) { return warning.find("read-only reference") != std::string::npos; }
            ),
            "External world source was not reported as a read-only reference."
        );

        const auto targetBefore   = readFile(targetManifest);
        const auto listBefore     = readFile(externalList);
        bool       versionBlocked = false;
        try {
            (void
            )mcdk::project::bumpProjectVersion(workspace, mcdk::project::VersionPart::Patch, targetManifest, false);
        } catch (const mcdk::project::ProjectError& error) {
            versionBlocked = error.code() == mcdk::project::ProjectErrorCode::OutOfScopeReference;
        }
        require(versionBlocked, "External world version reference did not block the target mutation.");
        require(readFile(targetManifest) == targetBefore, "Blocked version mutation changed the target.");
        require(readFile(externalList) == listBefore, "Blocked version mutation changed the external world list.");

        bool uuidBlocked = false;
        try {
            (void)mcdk::project::regenerateProjectUuids(workspace, targetManifest, false);
        } catch (const mcdk::project::ProjectError& error) {
            uuidBlocked = error.code() == mcdk::project::ProjectErrorCode::OutOfScopeReference;
        }
        require(uuidBlocked, "External world UUID reference did not block the target mutation.");
        require(readFile(targetManifest) == targetBefore, "Blocked UUID mutation changed the target.");
        require(readFile(externalList) == listBefore, "Blocked UUID mutation changed the external world list.");

        writeFile(externalList, "[{\"pack_id\": \"87878787-8787-4787-8787-878787878787\", \"version\": [1, 0, 0]}]\n");
        const auto unrelatedBefore = readFile(externalList);
        const auto bumped =
            mcdk::project::bumpProjectVersion(workspace, mcdk::project::VersionPart::Patch, targetManifest, false);
        require(bumped.modifiedFiles.size() == 1, "Unrelated external world reference changed the mutation scope.");
        require(readFile(targetManifest).find("[1, 0, 1]") != std::string::npos, "Target version did not change.");
        require(
            readFile(externalList) == unrelatedBefore,
            "Unrelated external world list was written by the target mutation."
        );
    }

    void testDirectoryTargetSelectsOnlyItsManifestSubtree() {
        TemporaryDirectory temporary;
        const auto         workspace       = temporary.path / "recursive-target-workspace";
        const auto         targetDirectory = workspace / "selected";
        const auto         targetManifest  = targetDirectory / "deep" / "pack" / "manifest.json";
        const auto         siblingManifest = workspace / "sibling" / "manifest.json";
        writeFile(
            targetManifest,
            basicManifest(
                "Deep Target",
                "data",
                "81818181-8181-4181-8181-818181818181",
                "82828282-8282-4282-8282-828282828282",
                "[1, 0, 0]"
            )
        );
        writeFile(
            siblingManifest,
            basicManifest(
                "Sibling",
                "data",
                "83838383-8383-4383-8383-838383838383",
                "84848484-8484-4484-8484-848484848484",
                "[1, 0, 0]"
            )
        );
        const auto siblingBefore = readFile(siblingManifest);
        const auto result =
            mcdk::project::bumpProjectVersion(workspace, mcdk::project::VersionPart::Patch, targetDirectory, false);
        require(result.modifiedFiles.size() == 1, "Directory target changed files outside its manifest subtree.");
        require(
            result.project.root == fs::canonical(targetDirectory),
            "Directory target summary kept the workspace root."
        );
        require(result.project.manifests.size() == 1, "Directory target summary included its sibling Mod.");
        require(readFile(targetManifest).find("[1, 0, 1]") != std::string::npos, "Deep target was not selected.");
        require(readFile(siblingManifest) == siblingBefore, "Sibling manifest header/modules changed with target.");
    }

    void testPreviewTamperStaleAndSizeLimit() {
        TemporaryDirectory temporary;
        const auto         root         = temporary.path / "preview-validation";
        const auto         manifestPath = root / "manifest.json";
        writeFile(
            manifestPath,
            basicManifest(
                "Preview Validation",
                "data",
                "61616161-6161-4161-8161-616161616161",
                "62626262-6262-4262-8262-626262626262",
                "[1, 0, 0]"
            )
        );
        const auto before   = readFile(manifestPath);
        const auto prepared = mcdk::project::regenerateProjectUuids(root, std::nullopt, true);
        require(prepared.preview.has_value(), "UUID regeneration preview was not returned.");

        auto tamperedDiff = *prepared.preview;
        tamperedDiff.files.front().afterContent.push_back(' ');
        bool invalidDiff = false;
        try {
            (void)mcdk::project::applyProjectPreview(root, tamperedDiff);
        } catch (const mcdk::project::ProjectError& error) {
            invalidDiff = error.code() == mcdk::project::ProjectErrorCode::InvalidPreview;
        }
        require(invalidDiff, "Tampered preview content was not rejected as invalid_preview.");
        require(readFile(manifestPath) == before, "Tampered preview changed the source file.");

        auto tamperedApproval                   = *prepared.preview;
        tamperedApproval.opaqueApproval.front() = tamperedApproval.opaqueApproval.front() == '0' ? '1' : '0';
        bool invalidApproval                    = false;
        try {
            (void)mcdk::project::applyProjectPreview(root, tamperedApproval);
        } catch (const mcdk::project::ProjectError& error) {
            invalidApproval = error.code() == mcdk::project::ProjectErrorCode::InvalidPreview;
        }
        require(invalidApproval, "Tampered opaque approval was not rejected as invalid_preview.");

        const auto applied = mcdk::project::applyProjectPreview(root, *prepared.preview);
        require(applied.modifiedFiles.size() == 1, "Approved UUID preview changed the wrong file count.");
        require(
            readFile(manifestPath) == prepared.preview->files.front().afterContent,
            "Applied UUID bytes differ from the approved preview diff."
        );

        writeFile(manifestPath, before);
        const auto stalePrepared = mcdk::project::regenerateProjectUuids(root, std::nullopt, true);
        writeFile(manifestPath, before + " \n");
        bool stale = false;
        try {
            (void)mcdk::project::applyProjectPreview(root, *stalePrepared.preview);
        } catch (const mcdk::project::ProjectError& error) {
            stale = error.code() == mcdk::project::ProjectErrorCode::PreviewStale;
        }
        require(stale, "Changed preview source bytes were not reported as preview_stale.");
        require(readFile(manifestPath) == before + " \n", "Stale preview overwrote a newer source file.");

        const auto hugeRoot = temporary.path / "preview-too-large";
        const auto hugePath = hugeRoot / "manifest.json";
        auto       huge     = basicManifest(
            "Huge Preview",
            "data",
            "71717171-7171-4171-8171-717171717171",
            "72727272-7272-4272-8272-727272727272",
            "[1, 0, 0]"
        );
        huge.append(mcdk::project::maxPreviewTotalBytes / 2 + 1, ' ');
        writeFile(hugePath, huge);
        bool tooLarge = false;
        try {
            (void)mcdk::project::bumpProjectVersion(hugeRoot, mcdk::project::VersionPart::Patch, std::nullopt, true);
        } catch (const mcdk::project::ProjectError& error) {
            tooLarge = error.code() == mcdk::project::ProjectErrorCode::PreviewTooLarge
                    && error.codeName() == "preview_too_large";
        }
        require(tooLarge, "A preview over 8 MiB was not rejected.");
        require(readFile(hugePath) == huge, "Oversized preview changed the source file.");
    }

    void testCrossProcessLockContentionIsReportedWithoutWrites() {
        TemporaryDirectory temporary;
        const auto         root         = temporary.path / "locked";
        const auto         manifestPath = root / "manifest.json";
        writeFile(
            manifestPath,
            basicManifest(
                "Locked",
                "data",
                "17171717-1717-4717-8717-171717171717",
                "18181818-1818-4818-8818-181818181818"
            )
        );
        const auto before = readFile(manifestPath);

        HeldProjectLock held(root);
        bool            inspectBusy = false;
        try {
            (void)mcdk::project::inspectProject(root);
        } catch (const mcdk::project::ProjectError& error) {
            inspectBusy = error.code() == mcdk::project::ProjectErrorCode::Busy && error.codeName() == "busy";
        }
        require(inspectBusy, "Inspect did not participate in the project lock.");

        bool busy = false;
        try {
            (void)mcdk::project::regenerateProjectUuids(root);
        } catch (const mcdk::project::ProjectError& error) {
            busy = error.code() == mcdk::project::ProjectErrorCode::Busy;
        }
        require(busy, "Cross-process lock contention was not reported as busy.");
        require(readFile(manifestPath) == before, "Busy project operation changed the manifest.");
    }

} // namespace

int main() {
    try {
        testDiscoversSingleMultiAndConfiguredPacksWithoutFollowingLinks();
        testImplicitRootScanIgnoresDevelopmentOutputsUnlessExplicitlyIncluded();
        testAllVersionPartsUpdateHeaderAndModules();
        testWorldSourceTriStateControlsAutomaticMapDetection();
        testMapMutationIsLosslessAndSynchronized();
        testDuplicateHeaderUuidFailsBeforeWrites();
        testVersionOverflowFailsBeforeWrites();
        testMalformedAndMissingFieldsFailBeforeAnyWrite();
        testOccupiedBackupPathIsNeverReplacedOrDeleted();
#ifdef _WIN32
        testPreexistingWindowsWriterPreventsAnyTransaction();
        testPreexistingWindowsReaderWithoutDeleteSharePreventsAnyTransaction();
        testProtectedTransactionPathsRejectNoDeleteShareReaders();
#endif
        testExternalSaveBeforeCommitIsNeverOverwritten();
        testLaterExternalSaveAfterFirstClaimIsPreserved();
        testExternalTargetPreviewSynchronizesWorkspaceAndRollsBackAtomically();
        testReadOnlyExternalReferenceBlocksTargetMutation();
        testExternalWorldSourceReferencesAreReadOnly();
        testDirectoryTargetSelectsOnlyItsManifestSubtree();
        testPreviewTamperStaleAndSizeLimit();
        testCrossProcessLockContentionIsReportedWithoutWrites();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
