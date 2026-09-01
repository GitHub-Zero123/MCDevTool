#include <CLI11.hpp>
#include <mcdevtool/addon.h>
#include <mcdevtool/env.h>
#include <mcdevtool/utils.h>
#include <mcdk/project_operations.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

    using Json = nlohmann::json;
    namespace fs = std::filesystem;
    namespace project = mcdk::project;

    [[nodiscard]] fs::path utf8Path(const std::string& value) {
#ifdef _WIN32
        return fs::path(CLI::widen(value));
#else
        return fs::path(value);
#endif
    }

    [[nodiscard]] std::string pathJson(const fs::path& value) {
        return MCDevTool::Utils::pathToGenericUtf8(value);
    }

    void envInfo() {
        using MCDevTool::Utils::pathToGenericUtf8;
        std::cout << "MCStudioDownload: "
                  << pathToGenericUtf8(MCDevTool::autoSearchMCStudioDownloadGamePath().value_or("")) << '\n';
        std::cout << "Minecraft.Windows.exe: "
                  << pathToGenericUtf8(MCDevTool::autoMatchLatestGameExePath().value_or("")) << '\n';
        std::cout << "MinecraftPE_Netease: " << pathToGenericUtf8(MCDevTool::getMinecraftDataPath()) << '\n';
        std::cout << "games/com.netease: " << pathToGenericUtf8(MCDevTool::getGamesComNeteasePath()) << '\n';
        std::cout << "behavior_packs: " << pathToGenericUtf8(MCDevTool::getBehaviorPacksPath()) << '\n';
        std::cout << "resource_packs: " << pathToGenericUtf8(MCDevTool::getResourcePacksPath()) << '\n';
        std::cout << "dependencies_packs: " << pathToGenericUtf8(MCDevTool::getDependenciesPacksPath()) << '\n';
        std::cout << "minecraftWorlds: " << pathToGenericUtf8(MCDevTool::getMinecraftWorldsPath()) << '\n';
    }

    void createEmptyAddonProject(const std::string& requestedName) {
        namespace Addon = MCDevTool::Addon;
        namespace Utils = MCDevTool::Utils;

        auto name = requestedName;
        if (name == "auto") {
            name.clear();
        }

        auto [behaviorManifest, resourceManifest] = Addon::createEmptyAddonManifest(name, {1, 0, 0});
        const auto workPath = fs::current_path();
        auto       folderName = name;
        if (folderName.empty()) {
            folderName = Utils::createCompactUUID();
            folderName[0] = 'A';
        }

        const auto behaviorPackPath = workPath / (folderName + "B");
        const auto resourcePackPath = workPath / (folderName + "R");
        fs::create_directories(behaviorPackPath / "entities");
        fs::create_directories(resourcePackPath / "textures");

        {
            std::ofstream file(behaviorPackPath / "manifest.json", std::ios::binary);
            file << behaviorManifest;
        }
        {
            std::ofstream file(resourcePackPath / "manifest.json", std::ios::binary);
            file << resourceManifest;
        }

        std::cout << "创建成功:\n";
        std::cout << "行为包路径: " << Utils::pathToGenericUtf8(behaviorPackPath) << '\n';
        std::cout << "资源包路径: " << Utils::pathToGenericUtf8(resourcePackPath) << '\n';
    }

    [[nodiscard]] Json versionJson(const project::Version& version) {
        return Json::array({version[0], version[1], version[2]});
    }

    [[nodiscard]] std::string_view versionPartName(project::VersionPart part) noexcept {
        switch (part) {
        case project::VersionPart::Major:
            return "major";
        case project::VersionPart::Minor:
            return "minor";
        case project::VersionPart::Patch:
            return "patch";
        }
        return "patch";
    }

    [[nodiscard]] std::string_view cliMutationOperationName(project::MutationOperation operation) noexcept {
        switch (operation) {
        case project::MutationOperation::BumpVersion:
            return "bump-version";
        case project::MutationOperation::RegenerateUuids:
            return "regenerate-uuids";
        }
        return "unknown";
    }

    [[nodiscard]] Json previewJson(const project::MutationPreview& preview) {
        Json target = nullptr;
        if (preview.target) {
            target = pathJson(*preview.target);
        }

        Json versionPart = nullptr;
        if (preview.versionPart) {
            versionPart = versionPartName(*preview.versionPart);
        }

        Json files = Json::array();
        for (const auto& file : preview.files) {
            files.push_back({
                {"path", pathJson(file.path)},
                {"before", file.beforeContent},
                {"after", file.afterContent},
            });
        }

        return {
            {"id", preview.id},
            {"operation", std::string(cliMutationOperationName(preview.operation))},
            {"root", pathJson(preview.root)},
            {"target", std::move(target)},
            {"version_part", std::move(versionPart)},
            {"files", std::move(files)},
            {"opaque_approval", preview.opaqueApproval},
        };
    }

    [[nodiscard]] Json projectJson(const project::ProjectSummary& summary) {
        Json manifests = Json::array();
        for (const auto& manifest : summary.manifests) {
            manifests.push_back({
                {"path", pathJson(manifest.path)},
                {"pack_directory", pathJson(manifest.packDirectory)},
                {"type", std::string(project::packKindName(manifest.kind))},
                {"name", manifest.name},
                {"uuid", manifest.uuid},
                {"version", versionJson(manifest.version)},
            });
        }

        Json packDirectories = Json::array();
        for (const auto& path : summary.packDirectories) {
            packDirectories.push_back(pathJson(path));
        }

        Json worldPackListFiles = Json::array();
        for (const auto& path : summary.worldPackListFiles) {
            worldPackListFiles.push_back(pathJson(path));
        }

        Json commonVersion = nullptr;
        if (!summary.manifests.empty()) {
            const auto& firstVersion = summary.manifests.front().version;
            const auto allMatch = std::all_of(summary.manifests.begin(), summary.manifests.end(), [&](const auto& item) {
                return item.version == firstVersion;
            });
            if (allMatch) {
                commonVersion = versionJson(firstVersion);
            }
        }

        return {
            {"root", pathJson(summary.root)},
            {"name", summary.name},
            {"type", std::string(project::projectKindName(summary.kind))},
            {"version", std::move(commonVersion)},
            {"behavior_pack_count", summary.behaviorPackCount()},
            {"resource_pack_count", summary.resourcePackCount()},
            {"manifests", std::move(manifests)},
            {"pack_directories", std::move(packDirectories)},
            {"world_pack_list_files", std::move(worldPackListFiles)},
            {"warnings", summary.warnings},
        };
    }

    [[nodiscard]] Json successJson(std::string_view operation, const project::OperationResult& result) {
        Json modifiedFiles = Json::array();
        if (!result.preview) {
            for (const auto& path : result.modifiedFiles) {
                modifiedFiles.push_back(pathJson(path));
            }
        }

        Json preview = nullptr;
        if (result.preview) {
            preview = previewJson(*result.preview);
        }

        return {
            {"protocol_version", project::protocolVersion},
            {"ok", true},
            {"operation", std::string(operation)},
            {"project", projectJson(result.project)},
            {"modified_files", std::move(modifiedFiles)},
            // Retained as an inert protocol v1 compatibility field. Project archive
            // creation is no longer supported, so successful operations always return null.
            {"archive_path", nullptr},
            {"warnings", result.warnings},
            {"preview", std::move(preview)},
        };
    }

    [[nodiscard]] Json errorJson(
        std::string_view operation,
        std::string_view code,
        std::string_view message,
        const std::optional<fs::path>& path = std::nullopt) {
        Json error = {{"code", std::string(code)}, {"message", std::string(message)}};
        if (path) {
            error["path"] = pathJson(*path);
        }
        return {
            {"protocol_version", project::protocolVersion},
            {"ok", false},
            {"operation", std::string(operation)},
            {"error", std::move(error)},
        };
    }

    void printWarnings(const std::vector<std::string>& warnings) {
        for (const auto& warning : warnings) {
            std::cerr << "warning: " << warning << '\n';
        }
    }

    void printHumanResult(std::string_view operation, const project::OperationResult& result) {
        const auto& summary = result.project;
        std::cout << "Project: " << summary.name << '\n';
        std::cout << "Root: " << pathJson(summary.root) << '\n';
        std::cout << "Type: " << project::projectKindName(summary.kind) << '\n';
        std::cout << "Behavior packs: " << summary.behaviorPackCount() << '\n';
        std::cout << "Resource packs: " << summary.resourcePackCount() << '\n';

        if (result.preview) {
            std::cout << "Preview: " << result.preview->id << '\n';
            for (const auto& file : result.preview->files) {
                std::cout << "Would modify: " << pathJson(file.path) << '\n';
            }
        } else if (operation == "inspect") {
            for (const auto& manifest : summary.manifests) {
                std::cout << "- " << manifest.name << " [" << manifest.version[0] << '.' << manifest.version[1] << '.'
                          << manifest.version[2] << "] " << pathJson(manifest.path) << '\n';
            }
        } else {
            for (const auto& path : result.modifiedFiles) {
                std::cout << "Modified: " << pathJson(path) << '\n';
            }
        }
        printWarnings(result.warnings);
    }

    [[nodiscard]] project::OperationResult
    inspectResult(const fs::path& root, const std::optional<fs::path>& target = std::nullopt) {
        auto summary = project::inspectProject(root, target);
        project::OperationResult result;
        result.warnings = summary.warnings;
        result.project = std::move(summary);
        return result;
    }

    [[nodiscard]] project::VersionPart parseVersionPart(const std::string& value) {
        if (value == "major") {
            return project::VersionPart::Major;
        }
        if (value == "minor") {
            return project::VersionPart::Minor;
        }
        return project::VersionPart::Patch;
    }

    [[nodiscard]] std::optional<fs::path> suppliedPath(const CLI::Option* option, const std::string& value) {
        if (option->count() == 0) {
            return std::nullopt;
        }
        return utf8Path(value);
    }

    [[noreturn]] void invalidPreview(std::string message) {
        throw project::ProjectError(project::ProjectErrorCode::InvalidPreview, std::move(message));
    }

    [[nodiscard]] const Json& requiredPreviewField(const Json& object, std::string_view name) {
        const auto iterator = object.find(std::string(name));
        if (iterator == object.end()) {
            invalidPreview("Preview is missing required field '" + std::string(name) + "'.");
        }
        return *iterator;
    }

    [[nodiscard]] const std::string& requiredPreviewString(const Json& object, std::string_view name) {
        const auto& value = requiredPreviewField(object, name);
        if (!value.is_string()) {
            invalidPreview("Preview field '" + std::string(name) + "' must be a string.");
        }
        return value.get_ref<const std::string&>();
    }

    [[nodiscard]] project::MutationOperation parseMutationOperation(const std::string& value) {
        if (value == "bump-version") {
            return project::MutationOperation::BumpVersion;
        }
        if (value == "regenerate-uuids") {
            return project::MutationOperation::RegenerateUuids;
        }
        invalidPreview("Preview field 'operation' has an unsupported value.");
    }

    [[nodiscard]] std::optional<project::VersionPart> parsePreviewVersionPart(const Json& value) {
        if (value.is_null()) {
            return std::nullopt;
        }
        if (!value.is_string()) {
            invalidPreview("Preview field 'version_part' must be null or a string.");
        }
        const auto& part = value.get_ref<const std::string&>();
        if (part != "major" && part != "minor" && part != "patch") {
            invalidPreview("Preview field 'version_part' has an unsupported value.");
        }
        return parseVersionPart(part);
    }

    [[nodiscard]] project::MutationPreview mutationPreviewFromJson(const Json& input) {
        if (!input.is_object()) {
            invalidPreview("Preview input must be a JSON object.");
        }

        const Json* previewObject = &input;
        const auto  envelopePreview = input.find("preview");
        if (envelopePreview != input.end()) {
            previewObject = &*envelopePreview;
        }
        if (!previewObject->is_object()) {
            invalidPreview("Preview must be a JSON object.");
        }

        project::MutationPreview preview;
        preview.id = requiredPreviewString(*previewObject, "id");
        preview.operation = parseMutationOperation(requiredPreviewString(*previewObject, "operation"));
        preview.root = utf8Path(requiredPreviewString(*previewObject, "root"));

        const auto& target = requiredPreviewField(*previewObject, "target");
        if (target.is_string()) {
            preview.target = utf8Path(target.get_ref<const std::string&>());
        } else if (!target.is_null()) {
            invalidPreview("Preview field 'target' must be null or a string.");
        }

        preview.versionPart = parsePreviewVersionPart(requiredPreviewField(*previewObject, "version_part"));
        preview.opaqueApproval = requiredPreviewString(*previewObject, "opaque_approval");

        const auto& files = requiredPreviewField(*previewObject, "files");
        if (!files.is_array()) {
            invalidPreview("Preview field 'files' must be an array.");
        }
        std::size_t totalBytes = 0;
        for (const auto& file : files) {
            if (!file.is_object()) {
                invalidPreview("Each preview file must be a JSON object.");
            }
            const auto& before = requiredPreviewString(file, "before");
            const auto& after = requiredPreviewString(file, "after");
            if (before.size() > project::maxPreviewTotalBytes - totalBytes) {
                throw project::ProjectError(
                    project::ProjectErrorCode::PreviewTooLarge,
                    "Preview file contents exceed the supported size limit."
                );
            }
            totalBytes += before.size();
            if (after.size() > project::maxPreviewTotalBytes - totalBytes) {
                throw project::ProjectError(
                    project::ProjectErrorCode::PreviewTooLarge,
                    "Preview file contents exceed the supported size limit."
                );
            }
            totalBytes += after.size();
            preview.files.push_back({
                .path = utf8Path(requiredPreviewString(file, "path")),
                .beforeContent = before,
                .afterContent = after,
            });
        }
        return preview;
    }

    [[nodiscard]] project::MutationPreview readMutationPreview() {
        const std::string input{
            std::istreambuf_iterator<char>(std::cin),
            std::istreambuf_iterator<char>(),
        };
        if (std::cin.bad()) {
            invalidPreview("Failed to read preview JSON from standard input.");
        }
        if (input.empty()) {
            invalidPreview("Preview JSON is required on standard input.");
        }

        try {
            return mutationPreviewFromJson(Json::parse(input));
        } catch (const project::ProjectError&) {
            throw;
        } catch (const Json::exception&) {
            invalidPreview("Standard input is not valid preview JSON.");
        }
    }

    [[nodiscard]] bool confirmUuidRegeneration() {
        std::cerr << "Regenerate all project manifest UUIDs and update internal references? [y/N] ";
        std::string answer;
        if (!std::getline(std::cin, answer)) {
            return false;
        }
        std::transform(answer.begin(), answer.end(), answer.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        return answer == "y" || answer == "yes";
    }

    [[nodiscard]] bool standardInputIsInteractive() noexcept {
#ifdef _WIN32
        return ::_isatty(::_fileno(stdin)) != 0;
#else
        return ::isatty(fileno(stdin)) != 0;
#endif
    }

    int reportBusinessError(
        bool json,
        std::string_view operation,
        std::string_view code,
        std::string_view message,
        const std::optional<fs::path>& path = std::nullopt) {
        if (json) {
            std::cout << errorJson(operation, code, message, path).dump() << '\n';
        } else {
            std::cerr << "error [" << code << "]: " << message;
            if (path) {
                std::cerr << " (" << pathJson(*path) << ')';
            }
            std::cerr << '\n';
        }
        return 1;
    }

    int runProjectOperation(
        std::string_view operation,
        bool json,
        const std::function<project::OperationResult()>& action) {
        try {
            auto result = action();
            if (json) {
                std::cout << successJson(operation, result).dump() << '\n';
            } else {
                printHumanResult(operation, result);
            }
            return 0;
        } catch (const project::ProjectError& error) {
            return reportBusinessError(json, operation, error.codeName(), error.what(), error.path());
        } catch (const std::exception& error) {
            return reportBusinessError(json, operation, "internal_error", error.what());
        }
    }

} // namespace

#ifdef _WIN32
int MCDK_CLI_PARSE(int argc, wchar_t* argv[]) {
#else
int MCDK_CLI_PARSE(int argc, char* argv[]) {
#endif
    CLI::App app{"MCDK CLI"};
    app.require_subcommand(1);

    auto* envInfoCommand = app.add_subcommand("envinfo", "列出当前环境信息");

    auto* createCommand = app.add_subcommand("create", "创建一个空的Addon项目");
    std::string createName = "auto";
    createCommand->add_option("-n,--name", createName, "项目名称")->default_val("auto");

    auto* projectCommand = app.add_subcommand("project", "Inspect and update the current project");
    projectCommand->require_subcommand(1);

    auto* inspectCommand = projectCommand->add_subcommand("inspect", "Inspect project manifests and configuration");
    std::string inspectRoot = ".";
    std::string inspectTarget;
    bool inspectJson = false;
    inspectCommand->add_option("--root", inspectRoot, "Project root directory")->default_val(".");
    auto* inspectTargetOption =
        inspectCommand->add_option("--target", inspectTarget, "Mod directory to inspect within the project");
    inspectCommand->add_flag("--json", inspectJson, "Write protocol v1 JSON to stdout");

    auto* uuidCommand = projectCommand->add_subcommand("regenerate-uuids", "Regenerate pack UUIDs and internal references");
    std::string uuidRoot = ".";
    std::string uuidTarget;
    bool uuidYes = false;
    bool uuidPreview = false;
    bool uuidJson = false;
    uuidCommand->add_option("--root", uuidRoot, "Project root directory")->default_val(".");
    auto* uuidTargetOption =
        uuidCommand->add_option("--target", uuidTarget, "Mod directory to update within the project");
    uuidCommand->add_flag("--preview", uuidPreview, "Return a reviewable preview without changing files");
    uuidCommand->add_flag("--yes", uuidYes, "Confirm UUID regeneration without prompting");
    uuidCommand->add_flag("--json", uuidJson, "Write protocol v1 JSON to stdout");

    auto* bumpCommand = projectCommand->add_subcommand("bump-version", "Increment project pack versions and references");
    std::string bumpRoot = ".";
    std::string bumpTarget;
    std::string bumpPart = "patch";
    bool bumpPreview = false;
    bool bumpJson = false;
    bumpCommand->add_option("--root", bumpRoot, "Project root directory")->default_val(".");
    auto* bumpTargetOption =
        bumpCommand->add_option("--target", bumpTarget, "Mod directory to update within the project");
    bumpCommand->add_option("--part", bumpPart, "Version part")
        ->check(CLI::IsMember({"patch", "minor", "major"}))
        ->default_val("patch");
    bumpCommand->add_flag("--preview", bumpPreview, "Return a reviewable preview without changing files");
    bumpCommand->add_flag("--json", bumpJson, "Write protocol v1 JSON to stdout");

    auto* applyPreviewCommand = projectCommand->add_subcommand(
        "apply-preview",
        "Apply a complete protocol v1 mutation preview read from standard input"
    );
    std::string applyPreviewRoot = ".";
    bool applyPreviewJson = false;
    applyPreviewCommand->add_option("--root", applyPreviewRoot, "Project root directory")->default_val(".");
    applyPreviewCommand->add_flag("--json", applyPreviewJson, "Write protocol v1 JSON to stdout");

    bool jsonRequested = false;
    std::string requestedOperation = "unknown";
    for (int index = 1; index < argc; ++index) {
#ifdef _WIN32
        const auto argument = CLI::narrow(argv[index]);
#else
        const std::string argument = argv[index];
#endif
        if (argument == "--json") {
            jsonRequested = true;
        }
        if (argument == "project" && index + 1 < argc) {
#ifdef _WIN32
            requestedOperation = CLI::narrow(argv[index + 1]);
#else
            requestedOperation = argv[index + 1];
#endif
        }
    }

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        const auto exitCode = error.get_exit_code();
        if (exitCode == 0) {
            (void)app.exit(error);
            return 0;
        }
        if (jsonRequested) {
            std::cout << errorJson(requestedOperation, "invalid_arguments", error.what()).dump() << '\n';
        } else {
            (void)app.exit(error);
        }
        return 2;
    }

    if (*envInfoCommand) {
        envInfo();
        return 0;
    }
    if (*createCommand) {
        createEmptyAddonProject(createName);
        return 0;
    }
    if (*inspectCommand) {
        return runProjectOperation("inspect", inspectJson, [&] {
            return inspectResult(utf8Path(inspectRoot), suppliedPath(inspectTargetOption, inspectTarget));
        });
    }
    if (*uuidCommand) {
        if (!uuidYes && !uuidPreview) {
            if (uuidJson || !standardInputIsInteractive()) {
                return reportBusinessError(
                    uuidJson,
                    "regenerate-uuids",
                    "confirmation_required",
                    "UUID regeneration requires --yes in non-interactive mode");
            }
            if (!confirmUuidRegeneration()) {
                return reportBusinessError(
                    false,
                    "regenerate-uuids",
                    "confirmation_required",
                    "UUID regeneration was not confirmed");
            }
        }
        return runProjectOperation("regenerate-uuids", uuidJson, [&] {
            return project::regenerateProjectUuids(
                utf8Path(uuidRoot),
                suppliedPath(uuidTargetOption, uuidTarget),
                uuidPreview
            );
        });
    }
    if (*bumpCommand) {
        return runProjectOperation("bump-version", bumpJson, [&] {
            return project::bumpProjectVersion(
                utf8Path(bumpRoot),
                parseVersionPart(bumpPart),
                suppliedPath(bumpTargetOption, bumpTarget),
                bumpPreview
            );
        });
    }
    if (*applyPreviewCommand) {
        return runProjectOperation("apply-preview", applyPreviewJson, [&] {
            return project::applyProjectPreview(utf8Path(applyPreviewRoot), readMutationPreview());
        });
    }
    return 2;
}
