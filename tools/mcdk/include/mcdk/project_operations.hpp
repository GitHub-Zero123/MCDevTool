#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mcdk::project {

    inline constexpr std::uint32_t protocolVersion      = 1;
    inline constexpr std::size_t   maxPreviewTotalBytes = 8 * 1024 * 1024; // 8 MiB

    enum class ProjectKind : std::uint8_t {
        SinglePack,
        Addon,
        Map,
    };

    enum class PackKind : std::uint8_t {
        Behavior,
        Resource,
        Unknown,
    };

    enum class VersionPart : std::uint8_t {
        Major,
        Minor,
        Patch,
    };

    enum class MutationOperation : std::uint8_t {
        BumpVersion,
        RegenerateUuids,
    };

    enum class ProjectErrorCode : std::uint8_t {
        InvalidProject,
        InvalidManifest,
        MissingField,
        DuplicateUuid,
        AmbiguousReference,
        VersionOverflow,
        SourceChanged,
        Busy,
        IoError,
        InvalidTarget,
        InvalidPreview,
        PreviewStale,
        PreviewTooLarge,
        OutOfScopeReference,
    };

    using Version = std::array<std::uint32_t, 3>;

    struct ManifestSummary {
        std::filesystem::path path;
        std::filesystem::path packDirectory;
        PackKind              kind = PackKind::Unknown;
        std::string           name;
        std::string           uuid;
        Version               version{};
    };

    struct ProjectSummary {
        std::filesystem::path                root;
        std::string                          name;
        ProjectKind                          kind = ProjectKind::Addon;
        std::vector<ManifestSummary>         manifests;
        std::vector<std::filesystem::path>   packDirectories;
        std::vector<std::filesystem::path>   worldPackListFiles;
        std::optional<std::filesystem::path> worldDirectory;
        std::vector<std::string>             warnings;
        [[nodiscard]] std::size_t behaviorPackCount() const noexcept;
        [[nodiscard]] std::size_t resourcePackCount() const noexcept;
    };

    struct PreviewFile {
        std::filesystem::path path;
        std::string           beforeContent;
        std::string           afterContent;
    };

    struct MutationPreview {
        std::string                          id;
        MutationOperation                    operation = MutationOperation::BumpVersion;
        std::filesystem::path                root;
        std::optional<std::filesystem::path> target;
        std::optional<VersionPart>           versionPart;
        std::vector<PreviewFile>             files;
        std::string                          opaqueApproval;
    };

    struct OperationResult {
        ProjectSummary                       project;
        std::vector<std::filesystem::path>   modifiedFiles;
        std::vector<std::string>             warnings;
        std::optional<MutationPreview>       preview;
    };

    struct TargetProjectContext {
        std::filesystem::path                root;
        std::optional<std::filesystem::path> target;
    };

    class ProjectError final : public std::runtime_error {
    public:
        ProjectError(ProjectErrorCode code, std::string message);
        ProjectError(ProjectErrorCode code, std::filesystem::path path, std::string message);

        [[nodiscard]] ProjectErrorCode                            code() const noexcept;
        [[nodiscard]] std::string_view                            codeName() const noexcept;
        [[nodiscard]] const std::optional<std::filesystem::path>& path() const noexcept;

    private:
        ProjectErrorCode                     errorCode_;
        std::optional<std::filesystem::path> path_;
    };

    [[nodiscard]] std::string_view projectKindName(ProjectKind kind) noexcept;
    [[nodiscard]] std::string_view packKindName(PackKind kind) noexcept;
    [[nodiscard]] std::string_view projectErrorCodeName(ProjectErrorCode code) noexcept;
    [[nodiscard]] std::string_view mutationOperationName(MutationOperation op) noexcept;

    [[nodiscard]] ProjectSummary inspectProject(const std::filesystem::path& root);
    [[nodiscard]] ProjectSummary
    inspectProject(const std::filesystem::path& root, const std::optional<std::filesystem::path>& target);
    [[nodiscard]] OperationResult regenerateProjectUuids(const std::filesystem::path& root);
    [[nodiscard]] OperationResult regenerateProjectUuids(
        const std::filesystem::path&                root,
        const std::optional<std::filesystem::path>& target,
        bool                                        preview = false
    );
    [[nodiscard]] OperationResult bumpProjectVersion(const std::filesystem::path& root, VersionPart part);
    [[nodiscard]] OperationResult bumpProjectVersion(
        const std::filesystem::path&                root,
        VersionPart                                 part,
        const std::optional<std::filesystem::path>& target,
        bool                                        preview = false
    );
    [[nodiscard]] OperationResult
    applyProjectPreview(const std::filesystem::path& root, const MutationPreview& preview);
} // namespace mcdk::project
