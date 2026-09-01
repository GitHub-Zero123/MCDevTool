#include <mcdk/project_operations.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace mcdk::project {
    namespace {
        namespace fs = std::filesystem;

        enum class JsonType : std::uint8_t {
            Object,
            Array,
            String,
            Number,
            Boolean,
            Null,
        };

        struct JsonNode;

        struct JsonProperty {
            std::string               name;
            std::unique_ptr<JsonNode> value;
        };

        struct JsonNode {
            JsonType                               type = JsonType::Null;
            std::size_t                            start{};
            std::size_t                            end{};
            std::string                            stringValue;
            bool                                   booleanValue{};
            std::vector<JsonProperty>              properties;
            std::vector<std::unique_ptr<JsonNode>> elements;
        };

        [[nodiscard]] std::string pathToUtf8(const fs::path& path) {
            const auto value = path.generic_u8string();
            return {value.begin(), value.end()};
        }

        [[nodiscard]] std::string lowercaseAscii(std::string value) {
            std::ranges::transform(value, value.begin(), [](unsigned char character) {
                if (character >= 'A' && character <= 'Z') {
                    return static_cast<char>(character + ('a' - 'A'));
                }
                return static_cast<char>(character);
            });
            return value;
        }

        class JsoncParser final {
        public:
            JsoncParser(std::string_view text, fs::path path, ProjectErrorCode errorCode)
            : text_(text),
              path_(std::move(path)),
              errorCode_(errorCode) {
                if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xef
                    && static_cast<unsigned char>(text_[1]) == 0xbb && static_cast<unsigned char>(text_[2]) == 0xbf) {
                    position_ = 3;
                }
            }

            [[nodiscard]] std::unique_ptr<JsonNode> parse() {
                skipTrivia();
                auto root = parseValue();
                skipTrivia();
                if (position_ != text_.size()) {
                    fail("Unexpected content after the JSON value.");
                }
                return root;
            }

        private:
            struct ParsedString {
                std::string value;
                std::size_t start{};
                std::size_t end{};
            };

            [[noreturn]] void fail(std::string message) const {
                std::size_t line   = 1;
                std::size_t column = 1;
                for (std::size_t index = 0; index < std::min(position_, text_.size()); ++index) {
                    if (text_[index] == '\n') {
                        ++line;
                        column = 1;
                    } else {
                        ++column;
                    }
                }
                message += " (line " + std::to_string(line) + ", column " + std::to_string(column) + ")";
                throw ProjectError(errorCode_, path_, std::move(message));
            }

            void skipTrivia() {
                while (position_ < text_.size()) {
                    const unsigned char character = static_cast<unsigned char>(text_[position_]);
                    if (std::isspace(character) != 0) {
                        ++position_;
                        continue;
                    }
                    if (text_[position_] != '/' || position_ + 1 >= text_.size()) {
                        return;
                    }
                    if (text_[position_ + 1] == '/') {
                        position_ += 2;
                        while (position_ < text_.size() && text_[position_] != '\n' && text_[position_] != '\r') {
                            ++position_;
                        }
                        continue;
                    }
                    if (text_[position_ + 1] == '*') {
                        position_ += 2;
                        while (position_ + 1 < text_.size() && !(text_[position_] == '*' && text_[position_ + 1] == '/')
                        ) {
                            ++position_;
                        }
                        if (position_ + 1 >= text_.size()) {
                            fail("Unterminated block comment.");
                        }
                        position_ += 2;
                        continue;
                    }
                    return;
                }
            }

            [[nodiscard]] std::unique_ptr<JsonNode> parseValue() {
                skipTrivia();
                if (position_ >= text_.size()) {
                    fail("Expected a JSON value.");
                }
                switch (text_[position_]) {
                case '{':
                    return parseObject();
                case '[':
                    return parseArray();
                case '"': {
                    auto value        = parseString();
                    auto node         = std::make_unique<JsonNode>();
                    node->type        = JsonType::String;
                    node->start       = value.start;
                    node->end         = value.end;
                    node->stringValue = std::move(value.value);
                    return node;
                }
                case 't':
                    return parseLiteral("true", JsonType::Boolean, true);
                case 'f':
                    return parseLiteral("false", JsonType::Boolean, false);
                case 'n':
                    return parseLiteral("null", JsonType::Null, false);
                default:
                    if (text_[position_] == '-' || std::isdigit(static_cast<unsigned char>(text_[position_])) != 0) {
                        return parseNumber();
                    }
                    fail("Expected a JSON value.");
                }
            }

            [[nodiscard]] std::unique_ptr<JsonNode> parseObject() {
                auto node   = std::make_unique<JsonNode>();
                node->type  = JsonType::Object;
                node->start = position_++;
                skipTrivia();
                if (consume('}')) {
                    node->end = position_;
                    return node;
                }

                while (true) {
                    skipTrivia();
                    if (position_ >= text_.size() || text_[position_] != '"') {
                        fail("Expected a quoted object property name.");
                    }
                    auto propertyName = parseString();
                    skipTrivia();
                    if (!consume(':')) {
                        fail("Expected ':' after an object property name.");
                    }
                    auto value = parseValue();
                    node->properties.push_back({std::move(propertyName.value), std::move(value)});
                    skipTrivia();
                    if (consume('}')) {
                        node->end = position_;
                        return node;
                    }
                    if (!consume(',')) {
                        fail("Expected ',' or '}' in an object.");
                    }
                    skipTrivia();
                    if (consume('}')) {
                        node->end = position_;
                        return node;
                    }
                }
            }

            [[nodiscard]] std::unique_ptr<JsonNode> parseArray() {
                auto node   = std::make_unique<JsonNode>();
                node->type  = JsonType::Array;
                node->start = position_++;
                skipTrivia();
                if (consume(']')) {
                    node->end = position_;
                    return node;
                }

                while (true) {
                    node->elements.push_back(parseValue());
                    skipTrivia();
                    if (consume(']')) {
                        node->end = position_;
                        return node;
                    }
                    if (!consume(',')) {
                        fail("Expected ',' or ']' in an array.");
                    }
                    skipTrivia();
                    if (consume(']')) {
                        node->end = position_;
                        return node;
                    }
                }
            }

            [[nodiscard]] std::unique_ptr<JsonNode> parseNumber() {
                const auto start = position_;
                consume('-');
                if (position_ >= text_.size()) {
                    fail("Incomplete JSON number.");
                }
                if (text_[position_] == '0') {
                    ++position_;
                    if (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_])) != 0) {
                        fail("A JSON number cannot contain a leading zero.");
                    }
                } else {
                    if (text_[position_] < '1' || text_[position_] > '9') {
                        fail("Invalid JSON number.");
                    }
                    while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_])) != 0
                    ) {
                        ++position_;
                    }
                }
                if (consume('.')) {
                    if (position_ >= text_.size() || std::isdigit(static_cast<unsigned char>(text_[position_])) == 0) {
                        fail("A fractional JSON number requires digits after '.'.");
                    }
                    while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_])) != 0
                    ) {
                        ++position_;
                    }
                }
                if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
                    ++position_;
                    if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) {
                        ++position_;
                    }
                    if (position_ >= text_.size() || std::isdigit(static_cast<unsigned char>(text_[position_])) == 0) {
                        fail("A JSON exponent requires digits.");
                    }
                    while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_])) != 0
                    ) {
                        ++position_;
                    }
                }
                auto node   = std::make_unique<JsonNode>();
                node->type  = JsonType::Number;
                node->start = start;
                node->end   = position_;
                return node;
            }

            [[nodiscard]] std::unique_ptr<JsonNode>
            parseLiteral(std::string_view literal, JsonType type, bool booleanValue) {
                const auto start = position_;
                if (text_.substr(position_, literal.size()) != literal) {
                    fail("Invalid JSON literal.");
                }
                position_          += literal.size();
                auto node           = std::make_unique<JsonNode>();
                node->type          = type;
                node->start         = start;
                node->end           = position_;
                node->booleanValue  = booleanValue;
                return node;
            }

            static void appendCodePoint(std::string& result, std::uint32_t codePoint) {
                if (codePoint <= 0x7f) {
                    result.push_back(static_cast<char>(codePoint));
                } else if (codePoint <= 0x7ff) {
                    result.push_back(static_cast<char>(0xc0 | (codePoint >> 6)));
                    result.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
                } else if (codePoint <= 0xffff) {
                    result.push_back(static_cast<char>(0xe0 | (codePoint >> 12)));
                    result.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
                    result.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
                } else {
                    result.push_back(static_cast<char>(0xf0 | (codePoint >> 18)));
                    result.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f)));
                    result.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
                    result.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
                }
            }

            [[nodiscard]] std::uint32_t parseHexQuad() {
                if (position_ + 4 > text_.size()) {
                    fail("Incomplete JSON unicode escape.");
                }
                std::uint32_t value = 0;
                for (int index = 0; index < 4; ++index) {
                    const char character   = text_[position_++];
                    value                <<= 4;
                    if (character >= '0' && character <= '9') {
                        value |= static_cast<std::uint32_t>(character - '0');
                    } else if (character >= 'a' && character <= 'f') {
                        value |= static_cast<std::uint32_t>(character - 'a' + 10);
                    } else if (character >= 'A' && character <= 'F') {
                        value |= static_cast<std::uint32_t>(character - 'A' + 10);
                    } else {
                        fail("Invalid hexadecimal digit in a JSON unicode escape.");
                    }
                }
                return value;
            }

            [[nodiscard]] ParsedString parseString() {
                ParsedString result;
                result.start = position_;
                if (!consume('"')) {
                    fail("Expected a JSON string.");
                }
                while (position_ < text_.size()) {
                    const unsigned char character = static_cast<unsigned char>(text_[position_++]);
                    if (character == '"') {
                        result.end = position_;
                        return result;
                    }
                    if (character < 0x20) {
                        fail("A JSON string contains an unescaped control character.");
                    }
                    if (character != '\\') {
                        result.value.push_back(static_cast<char>(character));
                        continue;
                    }
                    if (position_ >= text_.size()) {
                        fail("Incomplete JSON string escape.");
                    }
                    const char escaped = text_[position_++];
                    switch (escaped) {
                    case '"':
                        result.value.push_back('"');
                        break;
                    case '\\':
                        result.value.push_back('\\');
                        break;
                    case '/':
                        result.value.push_back('/');
                        break;
                    case 'b':
                        result.value.push_back('\b');
                        break;
                    case 'f':
                        result.value.push_back('\f');
                        break;
                    case 'n':
                        result.value.push_back('\n');
                        break;
                    case 'r':
                        result.value.push_back('\r');
                        break;
                    case 't':
                        result.value.push_back('\t');
                        break;
                    case 'u': {
                        auto codePoint = parseHexQuad();
                        if (codePoint >= 0xd800 && codePoint <= 0xdbff) {
                            if (position_ + 2 > text_.size() || text_[position_] != '\\'
                                || text_[position_ + 1] != 'u') {
                                fail("A high surrogate must be followed by a low surrogate.");
                            }
                            position_      += 2;
                            const auto low  = parseHexQuad();
                            if (low < 0xdc00 || low > 0xdfff) {
                                fail("Invalid low surrogate in a JSON unicode escape.");
                            }
                            codePoint = 0x10000 + ((codePoint - 0xd800) << 10) + (low - 0xdc00);
                        } else if (codePoint >= 0xdc00 && codePoint <= 0xdfff) {
                            fail("Unexpected low surrogate in a JSON unicode escape.");
                        }
                        appendCodePoint(result.value, codePoint);
                        break;
                    }
                    default:
                        fail("Invalid JSON string escape.");
                    }
                }
                fail("Unterminated JSON string.");
            }

            bool consume(char expected) {
                if (position_ < text_.size() && text_[position_] == expected) {
                    ++position_;
                    return true;
                }
                return false;
            }

            std::string_view text_;
            fs::path         path_;
            ProjectErrorCode errorCode_;
            std::size_t      position_{};
        };

        struct JsoncDocument {
            fs::path                  path;
            std::string               text;
            std::unique_ptr<JsonNode> root;

            [[nodiscard]] static JsoncDocument read(const fs::path& path, ProjectErrorCode errorCode) {
                std::ifstream input(path, std::ios::binary);
                if (!input) {
                    throw ProjectError(ProjectErrorCode::IoError, path, "Unable to open the JSON file.");
                }
                std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
                if (input.bad()) {
                    throw ProjectError(ProjectErrorCode::IoError, path, "Unable to read the JSON file.");
                }
                auto root = JsoncParser(text, path, errorCode).parse();
                return {path, std::move(text), std::move(root)};
            }
        };

        [[nodiscard]] const JsonNode*
        optionalProperty(const JsonNode& object, std::string_view name, const fs::path& path, ProjectErrorCode code) {
            if (object.type != JsonType::Object) {
                throw ProjectError(code, path, "Expected a JSON object.");
            }
            const JsonNode* found = nullptr;
            for (const auto& property : object.properties) {
                if (property.name != name) {
                    continue;
                }
                if (found != nullptr) {
                    throw ProjectError(code, path, "Duplicate JSON property: " + std::string(name));
                }
                found = property.value.get();
            }
            return found;
        }

        [[nodiscard]] const JsonNode* requireProperty(
            const JsonNode&  object,
            std::string_view name,
            const fs::path&  path,
            ProjectErrorCode wrongTypeCode = ProjectErrorCode::InvalidManifest
        ) {
            const auto* value = optionalProperty(object, name, path, wrongTypeCode);
            if (value == nullptr) {
                throw ProjectError(
                    ProjectErrorCode::MissingField,
                    path,
                    "Missing required field: " + std::string(name)
                );
            }
            return value;
        }

        [[nodiscard]] const JsonNode& requireType(
            const JsonNode*  value,
            JsonType         type,
            const fs::path&  path,
            std::string_view description,
            ProjectErrorCode code = ProjectErrorCode::InvalidManifest
        ) {
            if (value == nullptr || value->type != type) {
                throw ProjectError(code, path, std::string(description));
            }
            return *value;
        }

        [[nodiscard]] std::string normalizeUuid(const JsonNode& node, const fs::path& path) {
            const auto& stringNode = requireType(&node, JsonType::String, path, "A UUID must be a string.");
            const auto& value      = stringNode.stringValue;
            if (value.size() != 36 || value[8] != '-' || value[13] != '-' || value[18] != '-' || value[23] != '-') {
                throw ProjectError(ProjectErrorCode::InvalidManifest, path, "Invalid UUID value: " + value);
            }
            for (std::size_t index = 0; index < value.size(); ++index) {
                if (index == 8 || index == 13 || index == 18 || index == 23) {
                    continue;
                }
                if (std::isxdigit(static_cast<unsigned char>(value[index])) == 0) {
                    throw ProjectError(ProjectErrorCode::InvalidManifest, path, "Invalid UUID value: " + value);
                }
            }
            return lowercaseAscii(value);
        }

        struct VersionField {
            Version                        value{};
            std::array<const JsonNode*, 3> parts{};
        };

        [[nodiscard]] VersionField parseVersion(const JsonNode& node, const JsoncDocument& document) {
            const auto& array = requireType(
                &node,
                JsonType::Array,
                document.path,
                "A manifest version must be an array of three non-negative integers."
            );
            if (array.elements.size() != 3) {
                throw ProjectError(
                    ProjectErrorCode::InvalidManifest,
                    document.path,
                    "A manifest version must contain exactly three integers."
                );
            }
            VersionField result;
            for (std::size_t index = 0; index < 3; ++index) {
                const auto& part = requireType(
                    array.elements[index].get(),
                    JsonType::Number,
                    document.path,
                    "A manifest version contains a non-integer value."
                );
                const auto    lexeme = std::string_view(document.text).substr(part.start, part.end - part.start);
                std::uint64_t value{};
                const auto [end, error] = std::from_chars(lexeme.data(), lexeme.data() + lexeme.size(), value);
                if (error != std::errc{} || end != lexeme.data() + lexeme.size()
                    || value > std::numeric_limits<std::uint32_t>::max()) {
                    throw ProjectError(
                        ProjectErrorCode::InvalidManifest,
                        document.path,
                        "A manifest version contains an invalid integer."
                    );
                }
                result.value[index] = static_cast<std::uint32_t>(value);
                result.parts[index] = &part;
            }
            return result;
        }

        struct DependencyReference {
            const JsonNode* uuidNode{};
            std::string     uuid;
            VersionField    version;
        };

        struct ManifestDocument {
            JsoncDocument                    document;
            ManifestSummary                  summary;
            const JsonNode*                  headerUuidNode{};
            VersionField                     headerVersion;
            std::vector<const JsonNode*>     moduleUuidNodes;
            std::vector<VersionField>        moduleVersions;
            std::vector<DependencyReference> dependencies;
        };

        struct WorldReference {
            const JsonNode* idNode{};
            std::string     uuid;
            VersionField    version;
        };

        struct WorldDocument {
            JsoncDocument               document;
            std::vector<WorldReference> references;
        };

        struct LoadedProject {
            ProjectSummary                summary;
            std::vector<ManifestDocument> manifests;
            std::vector<ManifestDocument> externalManifests;
            std::vector<WorldDocument>    worldDocuments;
            std::vector<WorldDocument>    externalWorldDocuments;
            fs::path                      workspaceRoot;
            fs::path                      targetRoot;
            std::vector<std::size_t>      targetManifestIndices;
        };

        [[nodiscard]] ManifestDocument loadManifest(const fs::path& path) {
            auto        document = JsoncDocument::read(path, ProjectErrorCode::InvalidManifest);
            const auto& root =
                requireType(document.root.get(), JsonType::Object, path, "The manifest root must be an object.");
            const auto& header = requireType(
                requireProperty(root, "header", path),
                JsonType::Object,
                path,
                "manifest.header must be an object."
            );
            const auto& name = requireType(
                requireProperty(header, "name", path),
                JsonType::String,
                path,
                "manifest.header.name must be a string."
            );
            if (name.stringValue.empty()) {
                throw ProjectError(ProjectErrorCode::InvalidManifest, path, "manifest.header.name cannot be empty.");
            }
            const auto& headerUuid = requireType(
                requireProperty(header, "uuid", path),
                JsonType::String,
                path,
                "manifest.header.uuid must be a string."
            );
            const auto  headerUuidNormalized = normalizeUuid(headerUuid, path);
            const auto  headerVersion        = parseVersion(*requireProperty(header, "version", path), document);
            const auto& modules              = requireType(
                requireProperty(root, "modules", path),
                JsonType::Array,
                path,
                "manifest.modules must be an array."
            );
            if (modules.elements.empty()) {
                throw ProjectError(
                    ProjectErrorCode::MissingField,
                    path,
                    "manifest.modules must contain at least one module."
                );
            }

            ManifestDocument result;
            result.document              = std::move(document);
            result.headerUuidNode        = &headerUuid;
            result.headerVersion         = headerVersion;
            result.summary.path          = path;
            result.summary.packDirectory = path.parent_path();
            result.summary.name          = name.stringValue;
            result.summary.uuid          = headerUuidNormalized;
            result.summary.version       = headerVersion.value;

            bool sawResource = false;
            bool sawBehavior = false;
            for (const auto& moduleValue : modules.elements) {
                const auto& module =
                    requireType(moduleValue.get(), JsonType::Object, path, "Each manifest module must be an object.");
                const auto& type = requireType(
                    requireProperty(module, "type", path),
                    JsonType::String,
                    path,
                    "A module type must be a string."
                );
                if (type.stringValue.empty()) {
                    throw ProjectError(ProjectErrorCode::InvalidManifest, path, "A module type cannot be empty.");
                }
                const auto& uuid = requireType(
                    requireProperty(module, "uuid", path),
                    JsonType::String,
                    path,
                    "A module UUID must be a string."
                );
                (void)normalizeUuid(uuid, path);
                result.moduleUuidNodes.push_back(&uuid);
                result.moduleVersions.push_back(parseVersion(*requireProperty(module, "version", path), result.document)
                );
                if (type.stringValue == "resources") {
                    sawResource = true;
                } else {
                    sawBehavior = true;
                }
            }
            result.summary.kind = sawResource && !sawBehavior ? PackKind::Resource
                                : sawBehavior && !sawResource ? PackKind::Behavior
                                                              : PackKind::Unknown;

            if (const auto* dependencies =
                    optionalProperty(root, "dependencies", path, ProjectErrorCode::InvalidManifest)) {
                const auto& values =
                    requireType(dependencies, JsonType::Array, path, "manifest.dependencies must be an array.");
                for (const auto& dependencyValue : values.elements) {
                    const auto& dependency = requireType(
                        dependencyValue.get(),
                        JsonType::Object,
                        path,
                        "Each dependency must be an object."
                    );
                    const auto* uuid = optionalProperty(dependency, "uuid", path, ProjectErrorCode::InvalidManifest);
                    if (uuid == nullptr) {
                        continue;
                    }
                    const auto normalized = normalizeUuid(*uuid, path);
                    result.dependencies.push_back({
                        uuid,
                        normalized,
                        parseVersion(*requireProperty(dependency, "version", path), result.document),
                    });
                }
            }
            return result;
        }

        [[nodiscard]] WorldDocument loadWorldDocument(const fs::path& path) {
            auto        document = JsoncDocument::read(path, ProjectErrorCode::InvalidManifest);
            const auto& root =
                requireType(document.root.get(), JsonType::Array, path, "A world pack list must be an array.");
            WorldDocument result;
            result.document = std::move(document);
            for (const auto& entryValue : root.elements) {
                const auto& entry = requireType(
                    entryValue.get(),
                    JsonType::Object,
                    path,
                    "Each world pack list entry must be an object."
                );
                const auto& id = requireType(
                    requireProperty(entry, "pack_id", path),
                    JsonType::String,
                    path,
                    "A world pack list pack_id must be a string."
                );
                result.references.push_back({
                    &id,
                    normalizeUuid(id, path),
                    parseVersion(*requireProperty(entry, "version", path), result.document),
                });
            }
            return result;
        }

        [[nodiscard]] bool isReparsePoint(const fs::path& path) {
#ifdef _WIN32
            const auto attributes = GetFileAttributesW(path.c_str());
            return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
            std::error_code error;
            return fs::is_symlink(fs::symlink_status(path, error));
#endif
        }

        [[nodiscard]] bool isRegularFileWithoutLinks(const fs::path& path) {
            if (isReparsePoint(path)) {
                return false;
            }
            std::error_code error;
            return fs::is_regular_file(fs::symlink_status(path, error));
        }

        [[nodiscard]] bool isDirectoryWithoutLinks(const fs::path& path) {
            if (isReparsePoint(path)) {
                return false;
            }
            std::error_code error;
            return fs::is_directory(fs::symlink_status(path, error));
        }

        [[nodiscard]] bool containsAnyReparseComponent(const fs::path& absolutePath) {
            auto current = absolutePath.root_path();
            for (const auto& component : absolutePath.relative_path()) {
                current /= component;
                std::error_code error;
                const auto      status = fs::symlink_status(current, error);
                if (error || status.type() == fs::file_type::not_found) {
                    continue;
                }
                if (isReparsePoint(current)) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] fs::path canonicalProjectRoot(const fs::path& suppliedRoot) {
            std::error_code error;
            auto            absolute = fs::absolute(suppliedRoot.empty() ? fs::current_path() : suppliedRoot, error);
            if (error) {
                throw ProjectError(ProjectErrorCode::IoError, suppliedRoot, "Unable to resolve the project root.");
            }
            absolute = absolute.lexically_normal();
            if (containsAnyReparseComponent(absolute)) {
                throw ProjectError(
                    ProjectErrorCode::InvalidProject,
                    absolute,
                    "The project root cannot contain a link or reparse point."
                );
            }
            if (!isDirectoryWithoutLinks(absolute)) {
                throw ProjectError(
                    ProjectErrorCode::InvalidProject,
                    absolute,
                    "The project root is not an existing directory."
                );
            }
            auto canonical = fs::canonical(absolute, error);
            if (error) {
                throw ProjectError(ProjectErrorCode::IoError, absolute, "Unable to canonicalize the project root.");
            }
            return canonical;
        }

        [[nodiscard]] fs::path canonicalTargetRoot(const fs::path& suppliedTarget) {
            std::error_code error;
            auto absolute = fs::absolute(suppliedTarget.empty() ? fs::current_path() : suppliedTarget, error);
            if (error) {
                throw ProjectError(ProjectErrorCode::InvalidTarget, suppliedTarget, "Unable to resolve the target.");
            }
            absolute = absolute.lexically_normal();
            if (containsAnyReparseComponent(absolute)) {
                throw ProjectError(
                    ProjectErrorCode::InvalidTarget,
                    absolute,
                    "The selected target cannot contain a link or reparse point."
                );
            }
            if (isRegularFileWithoutLinks(absolute)) {
                if (lowercaseAscii(pathToUtf8(absolute.filename())) != "manifest.json") {
                    throw ProjectError(
                        ProjectErrorCode::InvalidTarget,
                        absolute,
                        "A selected target file must be manifest.json."
                    );
                }
                absolute = absolute.parent_path();
            }
            try {
                return canonicalProjectRoot(absolute);
            } catch (const ProjectError& error) {
                if (error.code() != ProjectErrorCode::InvalidProject) {
                    throw;
                }
                throw ProjectError(ProjectErrorCode::InvalidTarget, error.path().value_or(absolute), error.what());
            }
        }

        [[nodiscard]] bool pathComponentEqual(const fs::path& left, const fs::path& right) {
#ifdef _WIN32
            return lowercaseAscii(pathToUtf8(left)) == lowercaseAscii(pathToUtf8(right));
#else
            return left == right;
#endif
        }

        [[nodiscard]] bool isPathInside(const fs::path& child, const fs::path& root) {
            const auto normalizedChild = child.lexically_normal();
            const auto normalizedRoot  = root.lexically_normal();
            auto       childPart       = normalizedChild.begin();
            for (auto rootPart = normalizedRoot.begin(); rootPart != normalizedRoot.end(); ++rootPart, ++childPart) {
                if (childPart == normalizedChild.end() || !pathComponentEqual(*childPart, *rootPart)) {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool containsReparseComponent(const fs::path& root, const fs::path& target) {
            if (!isPathInside(target, root)) {
                return false;
            }
            auto relative = target.lexically_relative(root);
            auto current  = root;
            for (const auto& component : relative) {
                if (component == ".") {
                    continue;
                }
                current /= component;
                if (isReparsePoint(current)) {
                    return true;
                }
            }
            return false;
        }

        struct DiscoveryConfig {
            std::vector<fs::path>   includedDirectories;
            std::optional<fs::path> worldSource;
            bool                    worldSourceDisabled = false;
        };

        [[nodiscard]] DiscoveryConfig readDiscoveryConfig(const fs::path& root) {
            DiscoveryConfig result;
            const auto      configPath = root / ".mcdev.json";
            if (!isRegularFileWithoutLinks(configPath)) {
                return result;
            }

            auto        document = JsoncDocument::read(configPath, ProjectErrorCode::InvalidProject);
            const auto& object   = requireType(
                document.root.get(),
                JsonType::Object,
                configPath,
                "The .mcdev.json root must be an object.",
                ProjectErrorCode::InvalidProject
            );
            if (const auto* included =
                    optionalProperty(object, "included_mod_dirs", configPath, ProjectErrorCode::InvalidProject)) {
                const auto& entries = requireType(
                    included,
                    JsonType::Array,
                    configPath,
                    "included_mod_dirs must be an array.",
                    ProjectErrorCode::InvalidProject
                );
                for (const auto& entryValue : entries.elements) {
                    std::string configuredPath;
                    bool        enabled = true;
                    if (entryValue->type == JsonType::String) {
                        configuredPath = entryValue->stringValue;
                    } else if (entryValue->type == JsonType::Object) {
                        if (const auto* path =
                                optionalProperty(*entryValue, "path", configPath, ProjectErrorCode::InvalidProject)) {
                            configuredPath = requireType(
                                                 path,
                                                 JsonType::String,
                                                 configPath,
                                                 "An included mod directory path must be a string.",
                                                 ProjectErrorCode::InvalidProject
                            )
                                                 .stringValue;
                        } else {
                            configuredPath = "./";
                        }
                        if (const auto* enabledValue = optionalProperty(
                                *entryValue,
                                "enabled",
                                configPath,
                                ProjectErrorCode::InvalidProject
                            )) {
                            enabled = requireType(
                                          enabledValue,
                                          JsonType::Boolean,
                                          configPath,
                                          "An included mod directory enabled flag must be boolean.",
                                          ProjectErrorCode::InvalidProject
                            )
                                          .booleanValue;
                        }
                    } else {
                        throw ProjectError(
                            ProjectErrorCode::InvalidProject,
                            configPath,
                            "Each included_mod_dirs entry must be a string or object."
                        );
                    }
                    if (!enabled) {
                        continue;
                    }
                    auto path = fs::u8path(configuredPath);
                    result.includedDirectories.push_back((path.is_absolute() ? path : root / path).lexically_normal());
                }
            }

            if (const auto* world =
                    optionalProperty(object, "world_source_path", configPath, ProjectErrorCode::InvalidProject)) {
                if (world->type == JsonType::Null) {
                    result.worldSourceDisabled = true;
                } else {
                    const auto& value = requireType(
                                            world,
                                            JsonType::String,
                                            configPath,
                                            "world_source_path must be a path string, auto, or null.",
                                            ProjectErrorCode::InvalidProject
                    )
                                            .stringValue;
                    if (value.empty()) {
                        result.worldSourceDisabled = true;
                    } else if (value != "auto") {
                        auto path          = fs::u8path(value);
                        result.worldSource = (path.is_absolute() ? path : root / path).lexically_normal();
                    }
                }
            }

            return result;
        }

        class ManifestCandidateCollector final {
        public:
            enum class ScanMode : std::uint8_t {
                ImplicitWorkspaceRoot,
                ExplicitLocation,
            };

            explicit ManifestCandidateCollector(std::vector<std::string>& warnings) : warnings_(warnings) {}

            void scanLocation(const fs::path& directory, ScanMode mode, const fs::path& boundaryRoot) {
                if (!isDirectoryWithoutLinks(directory)) {
                    return;
                }
                addManifest(directory / "manifest.json", boundaryRoot);
                scanNamedPackContainer(directory / "behavior_packs", boundaryRoot);
                scanNamedPackContainer(directory / "resource_packs", boundaryRoot);

                std::error_code        error;
                fs::directory_iterator iterator(directory, fs::directory_options::skip_permission_denied, error);
                if (error) {
                    warnings_.push_back("Unable to scan configured directory: " + pathToUtf8(directory));
                    return;
                }
                for (const auto& entry : iterator) {
                    const auto path = entry.path();
                    if (isReparsePoint(path)) {
                        warnings_.push_back("Skipped linked directory entry: " + pathToUtf8(path));
                        continue;
                    }
                    std::error_code statusError;
                    if (!entry.is_directory(statusError)) {
                        continue;
                    }
                    if (mode == ScanMode::ImplicitWorkspaceRoot && isImplicitRootScanExcluded(path.filename())) {
                        continue;
                    }
                    addManifest(path / "manifest.json", boundaryRoot);
                }
            }

            void scanTree(const fs::path& directory, const fs::path& boundaryRoot) {
                if (!isDirectoryWithoutLinks(directory)) {
                    return;
                }
                addManifest(directory / "manifest.json", boundaryRoot);
                std::error_code                  error;
                fs::recursive_directory_iterator iterator(
                    directory,
                    fs::directory_options::skip_permission_denied,
                    error
                );
                if (error) {
                    warnings_.push_back("Unable to scan selected target tree: " + pathToUtf8(directory));
                    return;
                }
                const fs::recursive_directory_iterator end;
                while (iterator != end) {
                    const auto path = iterator->path();
                    if (isReparsePoint(path)) {
                        warnings_.push_back("Skipped linked selected-target entry: " + pathToUtf8(path));
                        std::error_code statusError;
                        if (iterator->is_directory(statusError)) {
                            iterator.disable_recursion_pending();
                        }
                    } else {
                        std::error_code statusError;
                        if (iterator->is_directory(statusError)) {
                            addManifest(path / "manifest.json", boundaryRoot);
                        }
                    }
                    iterator.increment(error);
                    if (error) {
                        warnings_.push_back("Unable to continue scanning selected target tree: " + pathToUtf8(path));
                        break;
                    }
                }
            }

            [[nodiscard]] std::vector<fs::path> take() {
                std::ranges::sort(paths_, [](const auto& left, const auto& right) {
                    return pathToUtf8(left) < pathToUtf8(right);
                });
                return std::move(paths_);
            }

        private:
            [[nodiscard]] static bool isImplicitRootScanExcluded(const fs::path& filename) {
                static constexpr std::array<std::string_view, 15> names{
                    ".git",
                    ".hg",
                    ".svn",
                    ".mcdk",
                    ".vscode",
                    "node_modules",
                    "build",
                    "out",
                    "dist",
                    "target",
                    ".cache",
                    ".pytest_cache",
                    ".mypy_cache",
                    ".ruff_cache",
                    "__pycache__",
                };
                const auto normalized = lowercaseAscii(pathToUtf8(filename));
                return std::ranges::find(names, normalized) != names.end();
            }

            void scanNamedPackContainer(const fs::path& container, const fs::path& boundaryRoot) {
                if (!isDirectoryWithoutLinks(container)) {
                    return;
                }
                std::error_code        error;
                fs::directory_iterator iterator(container, fs::directory_options::skip_permission_denied, error);
                if (error) {
                    warnings_.push_back("Unable to scan pack container: " + pathToUtf8(container));
                    return;
                }
                for (const auto& entry : iterator) {
                    const auto path = entry.path();
                    if (isReparsePoint(path)) {
                        warnings_.push_back("Skipped linked pack directory: " + pathToUtf8(path));
                        continue;
                    }
                    std::error_code statusError;
                    if (entry.is_directory(statusError)) {
                        addManifest(path / "manifest.json", boundaryRoot);
                    }
                }
            }

            void addManifest(const fs::path& path, const fs::path& boundaryRoot) {
                if (!isRegularFileWithoutLinks(path)) {
                    return;
                }
                const auto normalized = path.lexically_normal();
                if (!isPathInside(normalized, boundaryRoot) || containsReparseComponent(boundaryRoot, normalized)) {
                    warnings_.push_back("Skipped manifest outside the real scan tree: " + pathToUtf8(normalized));
                    return;
                }
#ifdef _WIN32
                const auto key = lowercaseAscii(pathToUtf8(normalized));
#else
                const auto key = pathToUtf8(normalized);
#endif
                if (seen_.insert(key).second) {
                    paths_.push_back(normalized);
                }
            }

            std::vector<std::string>&       warnings_;
            std::unordered_set<std::string> seen_;
            std::vector<fs::path>           paths_;
        };

        [[nodiscard]] bool hasWorldMarker(const fs::path& directory) {
            if (!isDirectoryWithoutLinks(directory)) {
                return false;
            }
            if (isRegularFileWithoutLinks(directory / "level.dat") || isDirectoryWithoutLinks(directory / "db")) {
                return true;
            }
            static constexpr std::array names{
                "world_behavior_packs.json",
                "world_resource_packs.json",
                "netease_world_behavior_packs.json",
                "netease_world_resource_packs.json",
            };
            return std::ranges::any_of(names, [&directory](std::string_view name) {
                return isRegularFileWithoutLinks(directory / name);
            });
        }

        [[nodiscard]] LoadedProject discoverProject(
            const fs::path&                suppliedRoot,
            const std::optional<fs::path>& suppliedTarget = std::nullopt
        ) {
            const auto    root   = canonicalProjectRoot(suppliedRoot);
            const auto    target = suppliedTarget ? canonicalTargetRoot(*suppliedTarget) : root;
            const auto    config = readDiscoveryConfig(root);
            LoadedProject loaded;
            loaded.workspaceRoot         = root;
            loaded.targetRoot            = target;
            loaded.summary.root          = root;
            loaded.summary.name          = pathToUtf8(root.filename());
            if (loaded.summary.name.empty()) {
                loaded.summary.name = "project";
            }

            ManifestCandidateCollector      collector(loaded.summary.warnings);
            std::unordered_set<std::string> scanned;
            auto                            scanDirectory = [&](const fs::path&                      configured,
                                     std::string_view                     settingName,
                                     ManifestCandidateCollector::ScanMode mode,
                                     bool                                 allowExternal) {
                std::error_code error;
                auto            absolute = fs::absolute(configured, error);
                if (error) {
                    loaded.summary.warnings.push_back(
                        "Unable to resolve configured " + std::string(settingName) + ": " + pathToUtf8(configured)
                    );
                    return;
                }
                absolute = absolute.lexically_normal();
                if (containsAnyReparseComponent(absolute)) {
                    loaded.summary.warnings.push_back(
                        "Ignored linked " + std::string(settingName) + ": " + pathToUtf8(absolute)
                    );
                    return;
                }
                if (!isDirectoryWithoutLinks(absolute)) {
                    loaded.summary.warnings.push_back(
                        "Configured " + std::string(settingName) + " does not exist: " + pathToUtf8(absolute)
                    );
                    return;
                }
                auto path = fs::canonical(absolute, error);
                if (error) {
                    loaded.summary.warnings.push_back(
                        "Unable to canonicalize configured " + std::string(settingName) + ": " + pathToUtf8(absolute)
                    );
                    return;
                }
                const bool external = !isPathInside(path, root);
                if (external && !allowExternal) {
                    loaded.summary.warnings.push_back(
                        "Ignored external " + std::string(settingName) + ": " + pathToUtf8(path)
                    );
                    return;
                }
                if (external && !suppliedTarget && settingName != "selected target") {
                    loaded.summary.warnings.push_back(
                        "Ignored external " + std::string(settingName)
                        + " for project output; scanned as read-only reference: " + pathToUtf8(path)
                    );
                }
                const auto boundary        = external ? path : root;
                const bool isWorkspaceRoot = pathComponentEqual(path, root);
                const auto effectiveMode =
                    mode == ManifestCandidateCollector::ScanMode::ExplicitLocation && isWorkspaceRoot
                                                   ? ManifestCandidateCollector::ScanMode::ImplicitWorkspaceRoot
                                                   : mode;
#ifdef _WIN32
                auto key = lowercaseAscii(pathToUtf8(path));
#else
                auto key = pathToUtf8(path);
#endif
                key += effectiveMode == ManifestCandidateCollector::ScanMode::ImplicitWorkspaceRoot ? "\nimplicit"
                                                                                                    : "\nexplicit";
                if (scanned.insert(key).second) {
                    collector.scanLocation(path, effectiveMode, boundary);
                }
            };

            scanDirectory(root, "workspace root", ManifestCandidateCollector::ScanMode::ImplicitWorkspaceRoot, false);
            for (const auto& directory : config.includedDirectories) {
                scanDirectory(
                    directory,
                    "included_mod_dirs path",
                    ManifestCandidateCollector::ScanMode::ExplicitLocation,
                    true
                );
            }
            scanDirectory(target, "selected target", ManifestCandidateCollector::ScanMode::ExplicitLocation, true);
            if (suppliedTarget) {
                collector.scanTree(target, target);
            }

            std::optional<fs::path> readOnlyWorldDirectory;
            std::optional<fs::path> writableTargetWorldDirectory;
            if (config.worldSource) {
                const auto world = config.worldSource->lexically_normal();
                if (!isPathInside(world, root)) {
                    std::error_code error;
                    if (containsAnyReparseComponent(world)) {
                        loaded.summary.warnings.push_back(
                            "Ignored linked external world_source_path: " + pathToUtf8(world)
                        );
                    } else if (!isDirectoryWithoutLinks(world)) {
                        loaded.summary.warnings.push_back(
                            "Configured world_source_path does not exist: " + pathToUtf8(world)
                        );
                    } else {
                        auto canonicalWorld = fs::canonical(world, error);
                        if (error) {
                            loaded.summary.warnings.push_back(
                                "Unable to canonicalize configured world_source_path: " + pathToUtf8(world)
                            );
                        } else if (isPathInside(canonicalWorld, target)) {
                            writableTargetWorldDirectory = std::move(canonicalWorld);
                        } else {
                            readOnlyWorldDirectory = std::move(canonicalWorld);
                            loaded.summary.warnings.push_back(
                                "Ignored external world_source_path for project output; scanned as read-only "
                                "reference: "
                                + pathToUtf8(*readOnlyWorldDirectory)
                            );
                        }
                    }
                } else if (containsReparseComponent(root, world)) {
                    loaded.summary.warnings.push_back("Ignored linked world_source_path: " + pathToUtf8(world));
                } else if (!isDirectoryWithoutLinks(world)) {
                    loaded.summary.warnings.push_back(
                        "Configured world_source_path does not exist: " + pathToUtf8(world)
                    );
                } else {
                    loaded.summary.worldDirectory = world;
                    scanDirectory(
                        world,
                        "world_source_path",
                        ManifestCandidateCollector::ScanMode::ExplicitLocation,
                        false
                    );
                }
            }
            if (!loaded.summary.worldDirectory && !config.worldSourceDisabled && !config.worldSource
                && hasWorldMarker(root)) {
                loaded.summary.worldDirectory = root;
            }

            const auto                                manifestPaths = collector.take();
            std::unordered_map<std::string, fs::path> headerPaths;
            for (const auto& path : manifestPaths) {
                auto manifest = loadManifest(path);
                if (const auto existing = headerPaths.find(manifest.summary.uuid); existing != headerPaths.end()) {
                    throw ProjectError(
                        ProjectErrorCode::DuplicateUuid,
                        path,
                        "Duplicate manifest header UUID also used by " + pathToUtf8(existing->second)
                    );
                }
                headerPaths.emplace(manifest.summary.uuid, path);
                const bool primary = isPathInside(path, root) || isPathInside(path, target);
                if (!primary) {
                    loaded.externalManifests.push_back(std::move(manifest));
                    continue;
                }
                const auto index = loaded.manifests.size();
                if (isPathInside(path, target)) {
                    loaded.targetManifestIndices.push_back(index);
                }
                loaded.summary.manifests.push_back(manifest.summary);
                loaded.summary.packDirectories.push_back(manifest.summary.packDirectory);
                loaded.manifests.push_back(std::move(manifest));
            }

            std::ranges::sort(loaded.summary.packDirectories, [](const auto& left, const auto& right) {
                return pathToUtf8(left) < pathToUtf8(right);
            });
            loaded.summary.packDirectories.erase(
                std::ranges::unique(loaded.summary.packDirectories).begin(),
                loaded.summary.packDirectories.end()
            );

            std::vector<fs::path> worldRoots;
            if (loaded.summary.worldDirectory) {
                worldRoots.push_back(*loaded.summary.worldDirectory);
            }
            const auto targetWorldDirectory = writableTargetWorldDirectory             ? writableTargetWorldDirectory
                                            : suppliedTarget && hasWorldMarker(target) ? std::optional<fs::path>{target}
                                                                                       : std::nullopt;
            if (targetWorldDirectory && std::ranges::none_of(worldRoots, [&](const auto& path) {
                    return pathComponentEqual(path, *targetWorldDirectory);
                })) {
                worldRoots.push_back(*targetWorldDirectory);
            }
            static constexpr std::array worldListNames{
                "world_behavior_packs.json",
                "world_resource_packs.json",
                "netease_world_behavior_packs.json",
                "netease_world_resource_packs.json",
            };
            std::unordered_set<std::string> worldFiles;
            const auto                      loadWorldRoot = [&](const fs::path& worldRoot, bool readOnly) {
                const auto& boundary = readOnly ? worldRoot : isPathInside(worldRoot, root) ? root : target;
                for (std::string_view name : worldListNames) {
                    const auto path = worldRoot / name;
                    if (!isRegularFileWithoutLinks(path) || containsReparseComponent(boundary, path)) {
                        continue;
                    }
#ifdef _WIN32
                    const auto key = lowercaseAscii(pathToUtf8(path));
#else
                    const auto key = pathToUtf8(path);
#endif
                    if (worldFiles.insert(key).second) {
                        auto document = loadWorldDocument(path);
                        if (readOnly) {
                            loaded.externalWorldDocuments.push_back(std::move(document));
                        } else {
                            loaded.summary.worldPackListFiles.push_back(path);
                            loaded.worldDocuments.push_back(std::move(document));
                        }
                    }
                }
            };
            for (const auto& worldRoot : worldRoots) {
                loadWorldRoot(worldRoot, false);
            }
            if (readOnlyWorldDirectory) {
                loadWorldRoot(*readOnlyWorldDirectory, true);
            }

            if (suppliedTarget) {
                loaded.summary.root = target;
                loaded.summary.name = pathToUtf8(target.filename());
                if (loaded.summary.name.empty()) {
                    loaded.summary.name = "project";
                }
                loaded.summary.manifests.erase(
                    std::remove_if(
                        loaded.summary.manifests.begin(),
                        loaded.summary.manifests.end(),
                        [&](const auto& manifest) { return !isPathInside(manifest.path, target); }
                    ),
                    loaded.summary.manifests.end()
                );
                loaded.summary.packDirectories.erase(
                    std::remove_if(
                        loaded.summary.packDirectories.begin(),
                        loaded.summary.packDirectories.end(),
                        [&](const auto& directory) { return !isPathInside(directory, target); }
                    ),
                    loaded.summary.packDirectories.end()
                );
                loaded.summary.worldPackListFiles.erase(
                    std::remove_if(
                        loaded.summary.worldPackListFiles.begin(),
                        loaded.summary.worldPackListFiles.end(),
                        [&](const auto& path) { return !isPathInside(path, target); }
                    ),
                    loaded.summary.worldPackListFiles.end()
                );
                loaded.summary.worldDirectory = targetWorldDirectory;
            }

            if (suppliedTarget && loaded.summary.manifests.empty() && !loaded.summary.worldDirectory) {
                throw ProjectError(
                    ProjectErrorCode::InvalidTarget,
                    target,
                    "No manifest.json or gameplay-map content was found in the selected target."
                );
            }
            if (!suppliedTarget && loaded.manifests.empty() && !loaded.summary.worldDirectory) {
                throw ProjectError(
                    ProjectErrorCode::InvalidProject,
                    root,
                    "No manifest.json or gameplay-map content was found in the workspace."
                );
            }
            loaded.summary.kind = loaded.summary.worldDirectory        ? ProjectKind::Map
                                : loaded.summary.manifests.size() == 1 ? ProjectKind::SinglePack
                                                                       : ProjectKind::Addon;
            return loaded;
        }

        [[nodiscard]] std::string createSecureUuid() {
            std::array<unsigned char, 16> bytes{};
#ifdef _WIN32
            const auto status = BCryptGenRandom(
                nullptr,
                bytes.data(),
                static_cast<ULONG>(bytes.size()),
                BCRYPT_USE_SYSTEM_PREFERRED_RNG
            );
            if (status < 0) {
                throw ProjectError(ProjectErrorCode::IoError, "The system random-number generator failed.");
            }
#else
            std::random_device random;
            for (auto& byte : bytes) {
                byte = static_cast<unsigned char>(random());
            }
#endif
            bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
            bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);

            static constexpr char digits[] = "0123456789abcdef";
            std::string           result;
            result.reserve(36);
            for (std::size_t index = 0; index < bytes.size(); ++index) {
                if (index == 4 || index == 6 || index == 8 || index == 10) {
                    result.push_back('-');
                }
                result.push_back(digits[bytes[index] >> 4]);
                result.push_back(digits[bytes[index] & 0x0f]);
            }
            return result;
        }

        [[nodiscard]] std::string createUniqueUuid(std::unordered_set<std::string>& used) {
            for (int attempt = 0; attempt < 32; ++attempt) {
                auto uuid = createSecureUuid();
                if (used.insert(uuid).second) {
                    return uuid;
                }
            }
            throw ProjectError(ProjectErrorCode::IoError, "Unable to generate a unique UUID.");
        }

        struct TextEdit {
            std::size_t start{};
            std::size_t end{};
            std::string replacement;
        };

        void addEdit(std::vector<TextEdit>& edits, const JsonNode& node, std::string replacement) {
            const auto existing = std::ranges::find_if(edits, [&node](const auto& edit) {
                return edit.start == node.start && edit.end == node.end;
            });
            if (existing != edits.end()) {
                if (existing->replacement != replacement) {
                    throw ProjectError(
                        ProjectErrorCode::AmbiguousReference,
                        "Conflicting edits target the same JSON value."
                    );
                }
                return;
            }
            edits.push_back({node.start, node.end, std::move(replacement)});
        }

        void addStringEdit(std::vector<TextEdit>& edits, const JsonNode& node, std::string_view replacement) {
            addEdit(edits, node, "\"" + std::string(replacement) + "\"");
        }

        void addVersionEdits(std::vector<TextEdit>& edits, const VersionField& field, const Version& version) {
            for (std::size_t index = 0; index < version.size(); ++index) {
                addEdit(edits, *field.parts[index], std::to_string(version[index]));
            }
        }

        [[nodiscard]] std::string applyEdits(const JsoncDocument& document, std::vector<TextEdit> edits) {
            std::ranges::sort(edits, [](const auto& left, const auto& right) { return left.start > right.start; });
            std::size_t nextStart = document.text.size();
            auto        result    = document.text;
            for (const auto& edit : edits) {
                if (edit.start > edit.end || edit.end > document.text.size() || edit.end > nextStart) {
                    throw ProjectError(
                        ProjectErrorCode::AmbiguousReference,
                        document.path,
                        "Overlapping or invalid lossless JSON edits were produced."
                    );
                }
                result.replace(edit.start, edit.end - edit.start, edit.replacement);
                nextStart = edit.start;
            }
            return result;
        }

        struct FileChange {
            fs::path    path;
            std::string expectedContent;
            std::string content;
        };

        class SourceFileGuard final {
        public:
#ifdef MCDK_PROJECT_TEST_HOOKS
            explicit SourceFileGuard(const fs::path& path, bool allowTestWrites) {
#else
            explicit SourceFileGuard(const fs::path& path) {
#endif
#ifdef _WIN32
                DWORD shareMode     = FILE_SHARE_READ | FILE_SHARE_DELETE;
                DWORD desiredAccess = GENERIC_READ | DELETE;
#ifdef MCDK_PROJECT_TEST_HOOKS
                if (allowTestWrites) {
                    shareMode     |= FILE_SHARE_WRITE;
                    desiredAccess  = GENERIC_READ;
                }
#endif
                handle_ = CreateFileW(
                    path.c_str(),
                    desiredAccess,
                    shareMode,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                    nullptr
                );
                if (handle_ == INVALID_HANDLE_VALUE || !readIdentity(handle_, identity_)
                    || (identity_.attributes
                        & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_READONLY))
                           != 0) {
                    close();
                    throwProtectionFailure(path);
                }
#else
                descriptor_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
                if (descriptor_ < 0 || ::flock(descriptor_, LOCK_EX | LOCK_NB) != 0
                    || ::fstat(descriptor_, &identity_) != 0 || !S_ISREG(identity_.st_mode)) {
                    close();
                    throwProtectionFailure(path);
                }
#endif
            }

            SourceFileGuard(const SourceFileGuard&)            = delete;
            SourceFileGuard& operator=(const SourceFileGuard&) = delete;

            SourceFileGuard(SourceFileGuard&& other) noexcept { moveFrom(other); }

            SourceFileGuard& operator=(SourceFileGuard&& other) noexcept {
                if (this != &other) {
                    close();
                    moveFrom(other);
                }
                return *this;
            }

            ~SourceFileGuard() { close(); }

            [[nodiscard]] bool matchesPath(const fs::path& path) const noexcept {
#ifdef _WIN32
                const auto candidate = CreateFileW(
                    path.c_str(),
                    FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                    nullptr
                );
                if (candidate == INVALID_HANDLE_VALUE) {
                    return false;
                }
                FileIdentity candidateIdentity{};
                const bool   result =
                    readIdentity(candidate, candidateIdentity)
                    && (candidateIdentity.attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0
                    && candidateIdentity.volumeSerial == identity_.volumeSerial
                    && candidateIdentity.fileIndexHigh == identity_.fileIndexHigh
                    && candidateIdentity.fileIndexLow == identity_.fileIndexLow;
                CloseHandle(candidate);
                return result;
#else
                struct stat candidate{};
                return ::lstat(path.c_str(), &candidate) == 0 && S_ISREG(candidate.st_mode)
                    && candidate.st_dev == identity_.st_dev && candidate.st_ino == identity_.st_ino;
#endif
            }

            [[nodiscard]] std::string readContent(const fs::path& reportedPath) const {
                std::string                 result;
                std::array<char, 64 * 1024> buffer{};
#ifdef _WIN32
                LARGE_INTEGER start{};
                if (!SetFilePointerEx(handle_, start, nullptr, FILE_BEGIN)) {
                    throwProtectionFailure(reportedPath);
                }
                while (true) {
                    DWORD read = 0;
                    if (!ReadFile(handle_, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
                        throwProtectionFailure(reportedPath);
                    }
                    if (read == 0) {
                        break;
                    }
                    result.append(buffer.data(), read);
                }
#else
                off_t offset = 0;
                while (true) {
                    const auto read = ::pread(descriptor_, buffer.data(), buffer.size(), offset);
                    if (read < 0 && errno == EINTR) {
                        continue;
                    }
                    if (read < 0) {
                        throwProtectionFailure(reportedPath);
                    }
                    if (read == 0) {
                        break;
                    }
                    result.append(buffer.data(), static_cast<std::size_t>(read));
                    offset += read;
                }
#endif
                return result;
            }

            [[nodiscard]] bool claimPathNoReplace(const fs::path& source, const fs::path& destination) noexcept {
                if (!matchesPath(source)) {
                    return false;
                }
#ifdef _WIN32
                if (!MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
                    return false;
                }
#else
                if (::link(source.c_str(), destination.c_str()) != 0) {
                    return false;
                }
                if (::unlink(source.c_str()) != 0) {
                    if (matchesPath(destination)) {
                        (void)::unlink(destination.c_str());
                    }
                    return false;
                }
#endif
                return matchesPath(destination);
            }

#ifdef _WIN32
            [[nodiscard]] bool setDeleteDisposition(bool remove) noexcept {
                FILE_DISPOSITION_INFO disposition{};
                disposition.DeleteFile = remove ? TRUE : FALSE;
                return SetFileInformationByHandle(
                           handle_,
                           FileDispositionInfo,
                           &disposition,
                           static_cast<DWORD>(sizeof(disposition))
                       )
                    != FALSE;
            }
#endif

        private:
            [[noreturn]] static void throwProtectionFailure(const fs::path& path) {
                throw ProjectError(
                    ProjectErrorCode::SourceChanged,
                    path,
                    "A project source file could not be protected from concurrent writes."
                );
            }

#ifdef _WIN32
            struct FileIdentity {
                DWORD volumeSerial  = 0;
                DWORD fileIndexHigh = 0;
                DWORD fileIndexLow  = 0;
                DWORD attributes    = 0;
            };

            [[nodiscard]] static bool readIdentity(HANDLE handle, FileIdentity& identity) noexcept {
                BY_HANDLE_FILE_INFORMATION information{};
                if (!GetFileInformationByHandle(handle, &information)) {
                    return false;
                }
                identity.volumeSerial  = information.dwVolumeSerialNumber;
                identity.fileIndexHigh = information.nFileIndexHigh;
                identity.fileIndexLow  = information.nFileIndexLow;
                identity.attributes    = information.dwFileAttributes;
                return true;
            }

            void close() noexcept {
                if (handle_ != INVALID_HANDLE_VALUE) {
                    CloseHandle(handle_);
                    handle_ = INVALID_HANDLE_VALUE;
                }
            }

            void moveFrom(SourceFileGuard& other) noexcept {
                handle_   = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
                identity_ = other.identity_;
            }

            HANDLE       handle_ = INVALID_HANDLE_VALUE;
            FileIdentity identity_{};
#else
            void close() noexcept {
                if (descriptor_ >= 0) {
                    ::flock(descriptor_, LOCK_UN);
                    ::close(descriptor_);
                    descriptor_ = -1;
                }
            }

            void moveFrom(SourceFileGuard& other) noexcept {
                descriptor_ = std::exchange(other.descriptor_, -1);
                identity_   = other.identity_;
            }

            int         descriptor_ = -1;
            struct stat identity_{};
#endif
        };

        class StagedFile final {
        public:
            StagedFile(fs::path path, std::string_view content, const fs::path& source) : path_(std::move(path)) {
#ifdef _WIN32
                handle_ = CreateFileW(
                    path_.c_str(),
                    GENERIC_READ | GENERIC_WRITE | DELETE,
                    FILE_SHARE_READ | FILE_SHARE_DELETE,
                    nullptr,
                    CREATE_NEW,
                    FILE_ATTRIBUTE_NORMAL,
                    nullptr
                );
                if (handle_ == INVALID_HANDLE_VALUE) {
                    throwWriteFailure(path_);
                }
                identityValid_ = readIdentity(handle_, identity_);
                if (!identityValid_ || !writeAll(content) || !FlushFileBuffers(handle_)) {
                    (void)markDelete();
                    close();
                    throwWriteFailure(path_);
                }
#else
                descriptor_ = ::open(path_.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
                if (descriptor_ < 0) {
                    throwWriteFailure(path_);
                }
                identityValid_ = ::fstat(descriptor_, &identity_) == 0 && S_ISREG(identity_.st_mode);
                if (!identityValid_ || ::flock(descriptor_, LOCK_EX | LOCK_NB) != 0 || !writeAll(content)
                    || ::fsync(descriptor_) != 0) {
                    (void)unlinkOwnedPath(path_);
                    close();
                    throwWriteFailure(path_);
                }
                struct stat sourceStatus{};
                if (::stat(source.c_str(), &sourceStatus) == 0) {
                    (void)::fchmod(descriptor_, sourceStatus.st_mode & 07777);
                }
#endif
            }

            StagedFile(const StagedFile&)            = delete;
            StagedFile& operator=(const StagedFile&) = delete;

            StagedFile(StagedFile&& other) noexcept { moveFrom(other); }

            StagedFile& operator=(StagedFile&& other) noexcept {
                if (this != &other) {
                    if (!published_) {
#ifdef _WIN32
                        (void)markDelete();
#else
                        (void)unlinkOwnedPath(path_);
#endif
                    }
                    close();
                    moveFrom(other);
                }
                return *this;
            }

            ~StagedFile() {
                if (!published_) {
#ifdef _WIN32
                    (void)markDelete();
#else
                    (void)unlinkOwnedPath(path_);
#endif
                }
                close();
            }

            [[nodiscard]] const fs::path& path() const noexcept { return path_; }

            [[nodiscard]] bool matchesPath(const fs::path& path) const noexcept {
                if (!identityValid_) {
                    return false;
                }
#ifdef _WIN32
                const auto candidate = CreateFileW(
                    path.c_str(),
                    FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                    nullptr
                );
                if (candidate == INVALID_HANDLE_VALUE) {
                    return false;
                }
                FileIdentity candidateIdentity{};
                const bool   result = readIdentity(candidate, candidateIdentity)
                                 && candidateIdentity.volumeSerial == identity_.volumeSerial
                                 && candidateIdentity.fileIndexHigh == identity_.fileIndexHigh
                                 && candidateIdentity.fileIndexLow == identity_.fileIndexLow;
                CloseHandle(candidate);
                return result;
#else
                struct stat candidate{};
                return ::lstat(path.c_str(), &candidate) == 0 && S_ISREG(candidate.st_mode)
                    && candidate.st_dev == identity_.st_dev && candidate.st_ino == identity_.st_ino;
#endif
            }

            [[nodiscard]] bool contentEquals(std::string_view expected) const noexcept {
                try {
                    return readContent() == expected;
                } catch (...) {
                    return false;
                }
            }

            [[nodiscard]] bool publish(const fs::path& target) noexcept {
#ifdef _WIN32
                if (!MoveFileExW(path_.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
                    return false;
                }
                published_ = true;
                return true;
#else
                if (::link(path_.c_str(), target.c_str()) != 0) {
                    return false;
                }
                if (::unlink(path_.c_str()) == 0) {
                    published_ = true;
                    return true;
                }
                if (matchesPath(target)) {
                    (void)::unlink(target.c_str());
                }
                return false;
#endif
            }

            [[nodiscard]] bool discardKnownPath(const fs::path& path) noexcept {
                if (!matchesPath(path)) {
                    return false;
                }
#ifdef _WIN32
                if (!markDelete()) {
                    return false;
                }
#else
                if (::unlink(path.c_str()) != 0) {
                    return false;
                }
#endif
                published_ = false;
                return true;
            }

        private:
            [[noreturn]] static void throwWriteFailure(const fs::path& path) {
                throw ProjectError(ProjectErrorCode::IoError, path, "Unable to write a protected staged project file.");
            }

            [[nodiscard]] std::string readContent() const {
                std::string                 result;
                std::array<char, 64 * 1024> buffer{};
#ifdef _WIN32
                LARGE_INTEGER start{};
                if (!SetFilePointerEx(handle_, start, nullptr, FILE_BEGIN)) {
                    throwWriteFailure(path_);
                }
                while (true) {
                    DWORD read = 0;
                    if (!ReadFile(handle_, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
                        throwWriteFailure(path_);
                    }
                    if (read == 0) {
                        break;
                    }
                    result.append(buffer.data(), read);
                }
#else
                off_t offset = 0;
                while (true) {
                    const auto read = ::pread(descriptor_, buffer.data(), buffer.size(), offset);
                    if (read < 0 && errno == EINTR) {
                        continue;
                    }
                    if (read < 0) {
                        throwWriteFailure(path_);
                    }
                    if (read == 0) {
                        break;
                    }
                    result.append(buffer.data(), static_cast<std::size_t>(read));
                    offset += read;
                }
#endif
                return result;
            }

#ifdef _WIN32
            struct FileIdentity {
                DWORD volumeSerial  = 0;
                DWORD fileIndexHigh = 0;
                DWORD fileIndexLow  = 0;
            };

            [[nodiscard]] static bool readIdentity(HANDLE handle, FileIdentity& identity) noexcept {
                BY_HANDLE_FILE_INFORMATION information{};
                if (!GetFileInformationByHandle(handle, &information)) {
                    return false;
                }
                identity.volumeSerial  = information.dwVolumeSerialNumber;
                identity.fileIndexHigh = information.nFileIndexHigh;
                identity.fileIndexLow  = information.nFileIndexLow;
                return true;
            }

            [[nodiscard]] bool markDelete() noexcept {
                if (deleteMarked_) {
                    return true;
                }
                if (handle_ == INVALID_HANDLE_VALUE) {
                    return false;
                }
                FILE_DISPOSITION_INFO disposition{};
                disposition.DeleteFile = TRUE;
                if (!SetFileInformationByHandle(
                        handle_,
                        FileDispositionInfo,
                        &disposition,
                        static_cast<DWORD>(sizeof(disposition))
                    )) {
                    return false;
                }
                deleteMarked_ = true;
                return true;
            }

            [[nodiscard]] bool writeAll(std::string_view content) noexcept {
                std::size_t writtenTotal = 0;
                while (writtenTotal < content.size()) {
                    const auto remaining = content.size() - writtenTotal;
                    const auto chunk     = static_cast<DWORD>(std::min<std::size_t>(remaining, 64 * 1024));
                    DWORD      written   = 0;
                    if (!WriteFile(handle_, content.data() + writtenTotal, chunk, &written, nullptr) || written == 0) {
                        return false;
                    }
                    writtenTotal += written;
                }
                return true;
            }

            void close() noexcept {
                if (handle_ != INVALID_HANDLE_VALUE) {
                    CloseHandle(handle_);
                    handle_ = INVALID_HANDLE_VALUE;
                }
            }

            void moveFrom(StagedFile& other) noexcept {
                path_          = std::move(other.path_);
                handle_        = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
                identity_      = other.identity_;
                identityValid_ = std::exchange(other.identityValid_, false);
                published_     = std::exchange(other.published_, false);
                deleteMarked_  = std::exchange(other.deleteMarked_, false);
            }

            HANDLE       handle_ = INVALID_HANDLE_VALUE;
            FileIdentity identity_{};
            bool         deleteMarked_ = false;
#else
            [[nodiscard]] bool writeAll(std::string_view content) noexcept {
                std::size_t writtenTotal = 0;
                while (writtenTotal < content.size()) {
                    const auto written =
                        ::write(descriptor_, content.data() + writtenTotal, content.size() - writtenTotal);
                    if (written < 0 && errno == EINTR) {
                        continue;
                    }
                    if (written <= 0) {
                        return false;
                    }
                    writtenTotal += static_cast<std::size_t>(written);
                }
                return true;
            }

            [[nodiscard]] bool unlinkOwnedPath(const fs::path& path) const noexcept {
                if (!matchesPath(path)) {
                    return false;
                }
                return ::unlink(path.c_str()) == 0;
            }

            void close() noexcept {
                if (descriptor_ >= 0) {
                    ::flock(descriptor_, LOCK_UN);
                    ::close(descriptor_);
                    descriptor_ = -1;
                }
            }

            void moveFrom(StagedFile& other) noexcept {
                path_          = std::move(other.path_);
                descriptor_    = std::exchange(other.descriptor_, -1);
                identity_      = other.identity_;
                identityValid_ = std::exchange(other.identityValid_, false);
                published_     = std::exchange(other.published_, false);
            }

            int         descriptor_ = -1;
            struct stat identity_{};
#endif
            fs::path path_;
            bool     identityValid_ = false;
            bool     published_     = false;
        };

        [[nodiscard]] std::uint64_t rootLockHash(const fs::path& root) {
            auto value = pathToUtf8(root.lexically_normal());
#ifdef _WIN32
            value = lowercaseAscii(std::move(value));
#endif
            std::uint64_t hash = 14695981039346656037ULL;
            for (const unsigned char character : value) {
                hash ^= character;
                hash *= 1099511628211ULL;
            }
            return hash;
        }

        class ProjectLock final {
        public:
            explicit ProjectLock(const fs::path& root) {
                std::error_code error;
                auto            directory = fs::temp_directory_path(error) / "mcdk-project-locks";
                if (error) {
                    throw ProjectError(
                        ProjectErrorCode::IoError,
                        root,
                        "Unable to find the temporary directory for project locking."
                    );
                }
                fs::create_directories(directory, error);
                if (error) {
                    throw ProjectError(
                        ProjectErrorCode::IoError,
                        directory,
                        "Unable to create the project lock directory."
                    );
                }
                std::ostringstream name;
                name << std::hex << std::setw(16) << std::setfill('0') << rootLockHash(root) << ".lock";
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
                    const auto code = GetLastError();
                    if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION) {
                        throw ProjectError(
                            ProjectErrorCode::Busy,
                            root,
                            "Another project operation is already running."
                        );
                    }
                    throw ProjectError(ProjectErrorCode::IoError, path_, "Unable to acquire the project lock.");
                }
#else
                descriptor_ = ::open(path_.c_str(), O_CREAT | O_RDWR, 0600);
                if (descriptor_ < 0) {
                    throw ProjectError(ProjectErrorCode::IoError, path_, "Unable to open the project lock.");
                }
                if (::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
                    const auto lockError = errno;
                    ::close(descriptor_);
                    descriptor_ = -1;
                    if (lockError == EWOULDBLOCK || lockError == EAGAIN) {
                        throw ProjectError(
                            ProjectErrorCode::Busy,
                            root,
                            "Another project operation is already running."
                        );
                    }
                    throw ProjectError(ProjectErrorCode::IoError, path_, "Unable to acquire the project lock.");
                }
#endif
            }

            ProjectLock(const ProjectLock&)            = delete;
            ProjectLock& operator=(const ProjectLock&) = delete;

            ~ProjectLock() {
#ifdef _WIN32
                if (handle_ != INVALID_HANDLE_VALUE) {
                    CloseHandle(handle_);
                }
#else
                if (descriptor_ >= 0) {
                    ::flock(descriptor_, LOCK_UN);
                    ::close(descriptor_);
                }
#endif
            }

        private:
            fs::path path_;
#ifdef _WIN32
            HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
            int descriptor_ = -1;
#endif
        };

        struct CanonicalMutationRoots {
            fs::path workspace;
            fs::path target;
            bool     explicitTarget = false;
        };

        [[nodiscard]] CanonicalMutationRoots
        resolveMutationRoots(const fs::path& root, const std::optional<fs::path>& target) {
            CanonicalMutationRoots result;
            result.workspace      = canonicalProjectRoot(root);
            result.target         = target ? canonicalTargetRoot(*target) : result.workspace;
            result.explicitTarget = target.has_value();
            return result;
        }

        [[nodiscard]] std::vector<std::unique_ptr<ProjectLock>>
        lockProjectRoots(const fs::path& workspace, const fs::path& target) {
            std::vector<fs::path> roots{workspace};
            if (!pathComponentEqual(workspace, target)) {
                roots.push_back(target);
            }
            std::ranges::sort(roots, [](const auto& left, const auto& right) {
#ifdef _WIN32
                return lowercaseAscii(pathToUtf8(left)) < lowercaseAscii(pathToUtf8(right));
#else
                return pathToUtf8(left) < pathToUtf8(right);
#endif
            });
            std::vector<std::unique_ptr<ProjectLock>> locks;
            locks.reserve(roots.size());
            for (const auto& root : roots) {
                locks.push_back(std::make_unique<ProjectLock>(root));
            }
            return locks;
        }

        [[nodiscard]] fs::path adjacentTemporaryPath(const fs::path& target, std::string_view suffix) {
            for (int attempt = 0; attempt < 32; ++attempt) {
                auto id = createSecureUuid();
                id.erase(std::remove(id.begin(), id.end(), '-'), id.end());
                const auto      candidate = target.parent_path() / (".mcdk-project-" + id + std::string(suffix));
                std::error_code error;
                if (!fs::exists(candidate, error)) {
                    return candidate;
                }
            }
            throw ProjectError(ProjectErrorCode::IoError, target, "Unable to allocate a temporary project file.");
        }

#ifdef MCDK_PROJECT_TEST_HOOKS
        [[nodiscard]] std::optional<std::size_t> injectedIndex(const char* variable) {
            const auto* value = std::getenv(variable);
            if (value == nullptr || *value == '\0') {
                return std::nullopt;
            }
            std::size_t result{};
            const auto  length      = std::char_traits<char>::length(value);
            const auto [end, error] = std::from_chars(value, value + length, result);
            if (error != std::errc{} || end != value + length) {
                return std::nullopt;
            }
            return result;
        }

        [[nodiscard]] std::optional<std::size_t> injectedCommitFailureIndex() {
            return injectedIndex("MCDK_TEST_FAIL_PROJECT_COMMIT_AFTER");
        }

        void applyInjectedSourceMutation(
            const std::vector<FileChange>& changes,
            const char*                    variable,
            std::string_view               marker
        ) {
            const auto index = injectedIndex(variable);
            if (!index || *index == 0 || *index > changes.size()) {
                return;
            }
            const auto&   target = changes[*index - 1].path;
            std::ofstream output(target, std::ios::binary | std::ios::app);
            if (!output) {
                throw ProjectError(ProjectErrorCode::IoError, target, "Unable to inject the external source change.");
            }
            output << marker;
            output.flush();
            if (!output) {
                throw ProjectError(ProjectErrorCode::IoError, target, "Unable to inject the external source change.");
            }
        }

#ifdef _WIN32
        void applyInjectedFileGuardProbe(const fs::path& path) {
            if (!injectedIndex("MCDK_TEST_PROBE_PROJECT_FILE_GUARDS")) {
                return;
            }
            const auto candidate = CreateFileW(
                path.c_str(),
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            );
            if (candidate != INVALID_HANDLE_VALUE) {
                CloseHandle(candidate);
                throw ProjectError(
                    ProjectErrorCode::IoError,
                    path,
                    "A no-delete-share reader bypassed a protected project transaction file."
                );
            }
            const auto error = GetLastError();
            if (error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION) {
                throw ProjectError(
                    ProjectErrorCode::IoError,
                    path,
                    "Unable to verify project file sharing protection."
                );
            }
        }
#endif
#endif

        [[nodiscard]] bool pathEntryExists(const fs::path& path) {
            std::error_code error;
            const auto      status = fs::symlink_status(path, error);
            return !error && status.type() != fs::file_type::not_found;
        }

        [[nodiscard]] bool pathIsAbsent(const fs::path& path) noexcept {
            std::error_code error;
            const auto      status = fs::symlink_status(path, error);
            return (!error && status.type() == fs::file_type::not_found)
                || error == std::errc::no_such_file_or_directory;
        }

        [[nodiscard]] bool removePathChecked(const fs::path& path) noexcept {
            if (pathIsAbsent(path)) {
                return true;
            }
            std::error_code error;
            if (!fs::remove(path, error) || error) {
                return false;
            }
            return pathIsAbsent(path);
        }

        void commitChanges(const std::vector<FileChange>& changes) {
            struct StagedChange {
                fs::path                    target;
                fs::path                    staged;
                fs::path                    backup;
                std::unique_ptr<StagedFile> file;
                bool                        originalMoved      = false;
                bool                        published          = false;
                bool                        backupDeleteMarked = false;
            };

            std::vector<StagedChange> staged;
            staged.reserve(changes.size());
            try {
                for (const auto& change : changes) {
                    if (!isRegularFileWithoutLinks(change.path)) {
                        throw ProjectError(
                            ProjectErrorCode::SourceChanged,
                            change.path,
                            "A project source file was removed, replaced, or changed into a link before commit."
                        );
                    }
                    StagedChange item;
                    item.target = change.path;
                    item.staged = adjacentTemporaryPath(change.path, ".tmp");
                    item.backup = adjacentTemporaryPath(change.path, ".rollback.tmp");
                    try {
                        item.file = std::make_unique<StagedFile>(item.staged, change.content, change.path);
#if defined(MCDK_PROJECT_TEST_HOOKS) && defined(_WIN32)
                        applyInjectedFileGuardProbe(item.staged);
#endif
                    } catch (...) {
                        item.file.reset();
                        throw;
                    }
                    staged.push_back(std::move(item));
                }
            } catch (...) {
                for (std::size_t index = 0; index < staged.size(); ++index) {
                    auto& item = staged[index];
                    item.file.reset();
                }
                throw;
            }

#ifdef MCDK_PROJECT_TEST_HOOKS
            const auto  failAfter      = injectedCommitFailureIndex();
            std::size_t publishedCount = 0;
            if (const auto occupiedBackup = injectedIndex("MCDK_TEST_OCCUPY_PROJECT_BACKUP_BEFORE_CLAIM");
                occupiedBackup && *occupiedBackup > 0 && *occupiedBackup <= staged.size()) {
                const auto&   path = staged[*occupiedBackup - 1].backup;
                std::ofstream output(path, std::ios::binary | std::ios::trunc);
                if (!output) {
                    throw ProjectError(ProjectErrorCode::IoError, path, "Unable to inject an occupied backup path.");
                }
                output << "external backup-path occupant\n";
                output.flush();
                if (!output) {
                    throw ProjectError(ProjectErrorCode::IoError, path, "Unable to inject an occupied backup path.");
                }
            }
#endif
            std::vector<SourceFileGuard> guards;
            const auto                   rollback = [&]() noexcept {
                for (std::size_t reverse = staged.size(); reverse > 0; --reverse) {
                    const auto      index  = reverse - 1;
                    auto&           item   = staged[index];
                    const auto&     change = changes[index];
                    std::error_code ignored;

                    bool targetExists = pathEntryExists(item.target);
                    if (item.published && targetExists) {
                        const bool unchangedPublished = item.file && item.file->matchesPath(item.target)
                                                     && item.file->contentEquals(change.content);
                        if (unchangedPublished && item.file->discardKnownPath(item.target)) {
                            item.file.reset();
                        }
                        targetExists = pathEntryExists(item.target);
                    }

                    const bool ownsBackup =
                        item.originalMoved && index < guards.size() && guards[index].matchesPath(item.backup);
                    if (ownsBackup) {
                        bool restoredBackup = false;
                        if (!targetExists) {
                            ignored.clear();
                            fs::create_hard_link(item.backup, item.target, ignored);
                            restoredBackup = !ignored && pathEntryExists(item.target);
                            if (!restoredBackup) {
                                ignored.clear();
                                fs::copy_file(item.backup, item.target, fs::copy_options::none, ignored);
                                restoredBackup = !ignored && pathEntryExists(item.target);
                            }
                            targetExists = pathEntryExists(item.target);
                        }
                        if (restoredBackup) {
                            ignored.clear();
                            fs::remove(item.backup, ignored);
                        }
                    }
                }
            };
            const auto validateGuard = [&](std::size_t index, const fs::path& currentPath) {
                if (!guards[index].matchesPath(currentPath)
                    || guards[index].readContent(changes[index].path) != changes[index].expectedContent) {
                    throw ProjectError(
                        ProjectErrorCode::SourceChanged,
                        changes[index].path,
                        "A project source file changed while the operation was committing."
                    );
                }
            };
            try {
#ifdef MCDK_PROJECT_TEST_HOOKS
                applyInjectedSourceMutation(
                    changes,
                    "MCDK_TEST_MUTATE_PROJECT_SOURCE_BEFORE_COMMIT",
                    "\n// simulated external save before project commit\n"
                );
#endif

                guards.reserve(changes.size());
#ifdef MCDK_PROJECT_TEST_HOOKS
                const bool allowAfterClaimTestWrite =
                    injectedIndex("MCDK_TEST_MUTATE_PROJECT_SOURCE_AFTER_FIRST_CLAIM").has_value();
                for (const auto& change : changes) {
                    guards.emplace_back(change.path, allowAfterClaimTestWrite);
                }
#else
                for (const auto& change : changes) {
                    guards.emplace_back(change.path);
                }
#endif
                for (std::size_t index = 0; index < changes.size(); ++index) {
                    validateGuard(index, changes[index].path);
                }

                for (std::size_t index = 0; index < staged.size(); ++index) {
                    auto& item = staged[index];
                    if (!guards[index].claimPathNoReplace(item.target, item.backup)) {
                        throw ProjectError(
                            ProjectErrorCode::SourceChanged,
                            item.target,
                            "A project source file could not be atomically claimed because it changed or is in use."
                        );
                    }
                    item.originalMoved = true;
#if defined(MCDK_PROJECT_TEST_HOOKS) && defined(_WIN32)
                    applyInjectedFileGuardProbe(item.backup);
#endif
                    validateGuard(index, item.backup);
#ifdef MCDK_PROJECT_TEST_HOOKS
                    if (index == 0) {
                        applyInjectedSourceMutation(
                            changes,
                            "MCDK_TEST_MUTATE_PROJECT_SOURCE_AFTER_FIRST_CLAIM",
                            "\n// simulated external save after first source claim\n"
                        );
                    }
#endif
                }

                for (std::size_t index = 0; index < staged.size(); ++index) {
                    auto& item = staged[index];
                    if (pathEntryExists(item.target)) {
                        throw ProjectError(
                            ProjectErrorCode::SourceChanged,
                            item.target,
                            "A project source path was recreated while the operation was committing."
                        );
                    }
                    validateGuard(index, item.backup);
                }

                for (std::size_t index = 0; index < staged.size(); ++index) {
                    auto& item = staged[index];
                    if (!item.file->publish(item.target)) {
                        item.published = item.file->matchesPath(item.target);
                        if (pathEntryExists(item.target)) {
                            throw ProjectError(
                                ProjectErrorCode::SourceChanged,
                                item.target,
                                "A project source path was recreated while the operation was publishing."
                            );
                        }
                        throw ProjectError(
                            ProjectErrorCode::IoError,
                            item.target,
                            "Unable to publish the staged project file."
                        );
                    }
                    item.published = true;
                    if (!item.file->matchesPath(item.target) || !item.file->contentEquals(changes[index].content)) {
                        throw ProjectError(
                            ProjectErrorCode::SourceChanged,
                            item.target,
                            "The published project file changed before the transaction completed."
                        );
                    }
#ifdef MCDK_PROJECT_TEST_HOOKS
                    ++publishedCount;
                    if (failAfter && publishedCount >= *failAfter) {
                        throw ProjectError(ProjectErrorCode::IoError, item.target, "Injected project commit failure.");
                    }
#endif
                }
                for (std::size_t index = 0; index < staged.size(); ++index) {
                    validateGuard(index, staged[index].backup);
                }
                for (const auto& item : staged) {
                    if (!pathIsAbsent(item.staged)) {
                        throw ProjectError(
                            ProjectErrorCode::IoError,
                            item.target,
                            "A protected project transaction path remained after publication."
                        );
                    }
                }
#ifdef _WIN32
                for (std::size_t index = 0; index < staged.size(); ++index) {
                    if (!guards[index].setDeleteDisposition(true)) {
                        throw ProjectError(
                            ProjectErrorCode::IoError,
                            staged[index].backup,
                            "Unable to delete a protected project backup."
                        );
                    }
                    staged[index].backupDeleteMarked = true;
                }
#else
                for (const auto& item : staged) {
                    if (!removePathChecked(item.backup)) {
                        throw ProjectError(
                            ProjectErrorCode::IoError,
                            item.backup,
                            "Unable to delete a protected project backup."
                        );
                    }
                }
#endif
            } catch (...) {
#ifdef _WIN32
                for (std::size_t index = 0; index < staged.size() && index < guards.size(); ++index) {
                    if (staged[index].backupDeleteMarked) {
                        (void)guards[index].setDeleteDisposition(false);
                        staged[index].backupDeleteMarked = false;
                    }
                }
#endif
                rollback();
                for (auto& item : staged) {
                    item.file.reset();
                }
                guards.clear();
                throw;
            }

            guards.clear();
            for (auto& item : staged) {
                item.file.reset();
            }
            for (const auto& item : staged) {
                if (!pathIsAbsent(item.backup) || !pathIsAbsent(item.staged)) {
                    throw ProjectError(
                        ProjectErrorCode::IoError,
                        item.target,
                        "A project transaction file remained after commit cleanup."
                    );
                }
            }
        }

        [[nodiscard]] Version bumpVersion(Version version, VersionPart part, const fs::path& path) {
            switch (part) {
            case VersionPart::Major:
                if (version[0] == std::numeric_limits<std::uint32_t>::max()) {
                    throw ProjectError(
                        ProjectErrorCode::VersionOverflow,
                        path,
                        "The manifest major version cannot be increased."
                    );
                }
                return {static_cast<std::uint32_t>(version[0] + 1), 0, 0};
            case VersionPart::Minor:
                if (version[1] == std::numeric_limits<std::uint32_t>::max()) {
                    throw ProjectError(
                        ProjectErrorCode::VersionOverflow,
                        path,
                        "The manifest minor version cannot be increased."
                    );
                }
                return {version[0], static_cast<std::uint32_t>(version[1] + 1), 0};
            case VersionPart::Patch:
                if (version[2] == std::numeric_limits<std::uint32_t>::max()) {
                    throw ProjectError(
                        ProjectErrorCode::VersionOverflow,
                        path,
                        "The manifest patch version cannot be increased."
                    );
                }
                return {version[0], version[1], static_cast<std::uint32_t>(version[2] + 1)};
            }
            throw ProjectError(ProjectErrorCode::InvalidManifest, path, "Unknown version part.");
        }

        struct UuidAssignment {
            fs::path    manifestPath;
            std::size_t slot{}; // 0 is header; module UUIDs start at 1.
            std::string before;
            std::string after;
        };

        struct MutationPlan {
            std::vector<FileChange>     changes;
            std::vector<fs::path>       modifiedFiles;
            std::vector<fs::path>       targetManifestPaths;
            std::vector<UuidAssignment> uuidAssignments;
        };

        [[noreturn]] void throwNoTargetManifests(const LoadedProject& loaded, bool explicitTarget) {
            throw ProjectError(
                explicitTarget ? ProjectErrorCode::InvalidTarget : ProjectErrorCode::InvalidProject,
                loaded.targetRoot,
                "The selected target has no manifests to update."
            );
        }

        [[nodiscard]] std::string manifestAssignmentKey(const fs::path& path, std::size_t slot) {
#ifdef _WIN32
            return lowercaseAscii(pathToUtf8(path.lexically_normal())) + "\n" + std::to_string(slot);
#else
            return pathToUtf8(path.lexically_normal()) + "\n" + std::to_string(slot);
#endif
        }

        [[nodiscard]] MutationPlan buildUuidPlan(
            const LoadedProject&               loaded,
            bool                               explicitTarget,
            const std::vector<UuidAssignment>* approvedAssignments = nullptr
        ) {
            if (loaded.targetManifestIndices.empty()) {
                throwNoTargetManifests(loaded, explicitTarget);
            }

            MutationPlan                    plan;
            std::unordered_set<std::string> used;
            const auto                      collectUsed = [&used](const ManifestDocument& manifest) {
                used.insert(manifest.summary.uuid);
                for (const auto* module : manifest.moduleUuidNodes) {
                    used.insert(lowercaseAscii(module->stringValue));
                }
            };
            for (const auto& manifest : loaded.manifests) {
                collectUsed(manifest);
            }
            for (const auto& manifest : loaded.externalManifests) {
                collectUsed(manifest);
            }

            std::unordered_map<std::string, const UuidAssignment*> approved;
            if (approvedAssignments != nullptr) {
                for (const auto& assignment : *approvedAssignments) {
                    const auto key = manifestAssignmentKey(assignment.manifestPath, assignment.slot);
                    if (!approved.emplace(key, &assignment).second) {
                        throw ProjectError(ProjectErrorCode::InvalidPreview, "Duplicate UUID approval assignment.");
                    }
                }
            }

            std::unordered_map<std::string, std::string> headerUuids;
            for (const auto index : loaded.targetManifestIndices) {
                const auto& manifest = loaded.manifests[index];
                plan.targetManifestPaths.push_back(manifest.document.path);
                const auto addAssignment = [&](std::size_t slot, std::string before) {
                    std::string after;
                    if (approvedAssignments == nullptr) {
                        after = createUniqueUuid(used);
                    } else {
                        const auto existing = approved.find(manifestAssignmentKey(manifest.document.path, slot));
                        if (existing == approved.end() || existing->second->before != before) {
                            throw ProjectError(
                                ProjectErrorCode::InvalidPreview,
                                manifest.document.path,
                                "The UUID approval mapping does not match the selected target."
                            );
                        }
                        after = lowercaseAscii(existing->second->after);
                        JsonNode node;
                        node.type        = JsonType::String;
                        node.stringValue = after;
                        (void)normalizeUuid(node, manifest.document.path);
                        if (!used.insert(after).second) {
                            throw ProjectError(
                                ProjectErrorCode::InvalidPreview,
                                manifest.document.path,
                                "The UUID approval mapping contains a duplicate UUID."
                            );
                        }
                        approved.erase(existing);
                    }
                    plan.uuidAssignments.push_back({manifest.document.path, slot, std::move(before), std::move(after)});
                };
                addAssignment(0, manifest.summary.uuid);
                for (std::size_t module = 0; module < manifest.moduleUuidNodes.size(); ++module) {
                    addAssignment(module + 1, lowercaseAscii(manifest.moduleUuidNodes[module]->stringValue));
                }
                headerUuids.emplace(
                    manifest.summary.uuid,
                    plan.uuidAssignments[plan.uuidAssignments.size() - manifest.moduleUuidNodes.size() - 1].after
                );
            }
            if (!approved.empty()) {
                throw ProjectError(ProjectErrorCode::InvalidPreview, "The UUID approval mapping has extra entries.");
            }

            std::unordered_map<std::string, const UuidAssignment*> assignmentByKey;
            for (const auto& assignment : plan.uuidAssignments) {
                assignmentByKey.emplace(manifestAssignmentKey(assignment.manifestPath, assignment.slot), &assignment);
            }
            std::unordered_set<std::size_t> targetIndices(
                loaded.targetManifestIndices.begin(),
                loaded.targetManifestIndices.end()
            );
            for (std::size_t index = 0; index < loaded.manifests.size(); ++index) {
                const auto&           manifest = loaded.manifests[index];
                std::vector<TextEdit> edits;
                if (targetIndices.contains(index)) {
                    addStringEdit(
                        edits,
                        *manifest.headerUuidNode,
                        assignmentByKey.at(manifestAssignmentKey(manifest.document.path, 0))->after
                    );
                    for (std::size_t module = 0; module < manifest.moduleUuidNodes.size(); ++module) {
                        addStringEdit(
                            edits,
                            *manifest.moduleUuidNodes[module],
                            assignmentByKey.at(manifestAssignmentKey(manifest.document.path, module + 1))->after
                        );
                    }
                }
                for (const auto& dependency : manifest.dependencies) {
                    if (const auto replacement = headerUuids.find(dependency.uuid); replacement != headerUuids.end()) {
                        addStringEdit(edits, *dependency.uuidNode, replacement->second);
                    }
                }
                if (!edits.empty()) {
                    plan.changes.push_back(
                        {manifest.document.path, manifest.document.text, applyEdits(manifest.document, std::move(edits))
                        }
                    );
                    plan.modifiedFiles.push_back(manifest.document.path);
                }
            }
            for (const auto& manifest : loaded.externalManifests) {
                for (const auto& dependency : manifest.dependencies) {
                    if (headerUuids.contains(dependency.uuid)) {
                        throw ProjectError(
                            ProjectErrorCode::OutOfScopeReference,
                            manifest.document.path,
                            "A read-only external manifest references a selected target UUID."
                        );
                    }
                }
            }
            for (const auto& world : loaded.externalWorldDocuments) {
                for (const auto& reference : world.references) {
                    if (headerUuids.contains(reference.uuid)) {
                        throw ProjectError(
                            ProjectErrorCode::OutOfScopeReference,
                            world.document.path,
                            "A read-only external world pack list references a selected target UUID."
                        );
                    }
                }
            }
            for (const auto& world : loaded.worldDocuments) {
                std::vector<TextEdit> edits;
                for (const auto& reference : world.references) {
                    if (const auto replacement = headerUuids.find(reference.uuid); replacement != headerUuids.end()) {
                        addStringEdit(edits, *reference.idNode, replacement->second);
                    }
                }
                if (!edits.empty()) {
                    plan.changes.push_back(
                        {world.document.path, world.document.text, applyEdits(world.document, std::move(edits))}
                    );
                    plan.modifiedFiles.push_back(world.document.path);
                }
            }
            return plan;
        }

        [[nodiscard]] MutationPlan
        buildVersionPlan(const LoadedProject& loaded, VersionPart part, bool explicitTarget) {
            if (loaded.targetManifestIndices.empty()) {
                throwNoTargetManifests(loaded, explicitTarget);
            }
            MutationPlan                             plan;
            std::unordered_map<std::string, Version> versions;
            for (const auto index : loaded.targetManifestIndices) {
                const auto& manifest = loaded.manifests[index];
                plan.targetManifestPaths.push_back(manifest.document.path);
                versions.emplace(
                    manifest.summary.uuid,
                    bumpVersion(manifest.headerVersion.value, part, manifest.document.path)
                );
            }
            std::unordered_set<std::size_t> targetIndices(
                loaded.targetManifestIndices.begin(),
                loaded.targetManifestIndices.end()
            );
            for (std::size_t index = 0; index < loaded.manifests.size(); ++index) {
                const auto&           manifest = loaded.manifests[index];
                std::vector<TextEdit> edits;
                if (targetIndices.contains(index)) {
                    const auto& ownVersion = versions.at(manifest.summary.uuid);
                    addVersionEdits(edits, manifest.headerVersion, ownVersion);
                    for (const auto& moduleVersion : manifest.moduleVersions) {
                        addVersionEdits(edits, moduleVersion, ownVersion);
                    }
                }
                for (const auto& dependency : manifest.dependencies) {
                    if (const auto replacement = versions.find(dependency.uuid); replacement != versions.end()) {
                        addVersionEdits(edits, dependency.version, replacement->second);
                    }
                }
                if (!edits.empty()) {
                    plan.changes.push_back(
                        {manifest.document.path, manifest.document.text, applyEdits(manifest.document, std::move(edits))
                        }
                    );
                    plan.modifiedFiles.push_back(manifest.document.path);
                }
            }
            for (const auto& manifest : loaded.externalManifests) {
                for (const auto& dependency : manifest.dependencies) {
                    if (versions.contains(dependency.uuid)) {
                        throw ProjectError(
                            ProjectErrorCode::OutOfScopeReference,
                            manifest.document.path,
                            "A read-only external manifest references a selected target version."
                        );
                    }
                }
            }
            for (const auto& world : loaded.externalWorldDocuments) {
                for (const auto& reference : world.references) {
                    if (versions.contains(reference.uuid)) {
                        throw ProjectError(
                            ProjectErrorCode::OutOfScopeReference,
                            world.document.path,
                            "A read-only external world pack list references a selected target version."
                        );
                    }
                }
            }
            for (const auto& world : loaded.worldDocuments) {
                std::vector<TextEdit> edits;
                for (const auto& reference : world.references) {
                    if (const auto replacement = versions.find(reference.uuid); replacement != versions.end()) {
                        addVersionEdits(edits, reference.version, replacement->second);
                    }
                }
                if (!edits.empty()) {
                    plan.changes.push_back(
                        {world.document.path, world.document.text, applyEdits(world.document, std::move(edits))}
                    );
                    plan.modifiedFiles.push_back(world.document.path);
                }
            }
            return plan;
        }

        [[nodiscard]] OperationResult resultAfterMutation(
            const fs::path&                root,
            const std::optional<fs::path>& target,
            std::vector<fs::path>          modifiedFiles
        ) {
            OperationResult result;
            result.project       = discoverProject(root, target).summary;
            result.modifiedFiles = std::move(modifiedFiles);
            result.warnings      = result.project.warnings;
            return result;
        }

        void appendUnsigned(std::string& output, std::uint64_t value) {
            for (std::size_t index = 0; index < sizeof(value); ++index) {
                output.push_back(static_cast<char>((value >> (index * 8U)) & 0xffU));
            }
        }

        void appendApprovalString(std::string& output, std::string_view value) {
            appendUnsigned(output, value.size());
            output.append(value);
        }

        class ApprovalReader final {
        public:
            explicit ApprovalReader(std::string_view input) : input_(input) {}

            [[nodiscard]] std::uint64_t readUnsigned() {
                if (position_ + sizeof(std::uint64_t) > input_.size()) {
                    fail();
                }
                std::uint64_t result = 0;
                for (std::size_t index = 0; index < sizeof(result); ++index) {
                    result |= static_cast<std::uint64_t>(static_cast<unsigned char>(input_[position_++]))
                           << (index * 8U);
                }
                return result;
            }

            [[nodiscard]] std::string readString() {
                const auto size = readUnsigned();
                if (size > input_.size() - position_) {
                    fail();
                }
                auto result  = std::string(input_.substr(position_, static_cast<std::size_t>(size)));
                position_   += static_cast<std::size_t>(size);
                return result;
            }

            void requireEnd() const {
                if (position_ != input_.size()) {
                    fail();
                }
            }

        private:
            [[noreturn]] static void fail() {
                throw ProjectError(ProjectErrorCode::InvalidPreview, "Malformed opaque preview approval.");
            }

            std::string_view input_;
            std::size_t      position_{};
        };

        [[nodiscard]] std::string hexEncode(std::string_view value) {
            static constexpr char digits[] = "0123456789abcdef";
            std::string           result;
            result.reserve(value.size() * 2);
            for (const unsigned char byte : value) {
                result.push_back(digits[byte >> 4U]);
                result.push_back(digits[byte & 0x0fU]);
            }
            return result;
        }

        [[nodiscard]] std::string hexDecode(std::string_view value) {
            if (value.size() % 2 != 0) {
                throw ProjectError(ProjectErrorCode::InvalidPreview, "Malformed opaque preview approval encoding.");
            }
            const auto digit = [](char character) -> int {
                if (character >= '0' && character <= '9') {
                    return character - '0';
                }
                if (character >= 'a' && character <= 'f') {
                    return character - 'a' + 10;
                }
                if (character >= 'A' && character <= 'F') {
                    return character - 'A' + 10;
                }
                return -1;
            };
            std::string result;
            result.reserve(value.size() / 2);
            for (std::size_t index = 0; index < value.size(); index += 2) {
                const int high = digit(value[index]);
                const int low  = digit(value[index + 1]);
                if (high < 0 || low < 0) {
                    throw ProjectError(ProjectErrorCode::InvalidPreview, "Malformed opaque preview approval encoding.");
                }
                result.push_back(static_cast<char>((high << 4) | low));
            }
            return result;
        }

        [[nodiscard]] std::string approvalDigest(std::string_view value) {
            static constexpr std::array<std::uint64_t, 4> offsets{
                14695981039346656037ULL,
                1099511628211ULL ^ 0x9e3779b97f4a7c15ULL,
                14695981039346656037ULL ^ 0xd6e8feb86659fd93ULL,
                1099511628211ULL ^ 0xa0761d6478bd642fULL,
            };
            static constexpr std::array<std::uint64_t, 4> primes{
                1099511628211ULL,
                1099511628211ULL + 2,
                1099511628211ULL + 6,
                1099511628211ULL + 12,
            };
            auto       hashes = offsets;
            const auto mix    = [&](std::string_view bytes) {
                for (const unsigned char byte : bytes) {
                    for (std::size_t lane = 0; lane < hashes.size(); ++lane) {
                        hashes[lane] ^= static_cast<std::uint64_t>(byte + lane * 17U);
                        hashes[lane] *= primes[lane];
                        hashes[lane] ^= hashes[lane] >> (13U + lane);
                    }
                }
            };
            mix("mcdk-mutation-preview-approval-v1:deterministic-cross-process");
            mix(value);
            std::string binary;
            for (const auto hash : hashes) {
                appendUnsigned(binary, hash);
            }
            return hexEncode(binary);
        }

        [[nodiscard]] std::string previewBinding(const MutationPreview& preview) {
            std::string value;
            appendUnsigned(value, protocolVersion);
            appendApprovalString(value, preview.id);
            appendUnsigned(value, static_cast<std::uint64_t>(preview.operation));
            appendApprovalString(value, pathToUtf8(preview.root.lexically_normal()));
            appendUnsigned(value, preview.target.has_value() ? 1 : 0);
            if (preview.target) {
                appendApprovalString(value, pathToUtf8(preview.target->lexically_normal()));
            }
            appendUnsigned(value, preview.versionPart.has_value() ? 1 : 0);
            if (preview.versionPart) {
                appendUnsigned(value, static_cast<std::uint64_t>(*preview.versionPart));
            }
            appendUnsigned(value, preview.files.size());
            for (const auto& file : preview.files) {
                appendApprovalString(value, pathToUtf8(file.path.lexically_normal()));
                appendApprovalString(value, file.beforeContent);
                appendApprovalString(value, file.afterContent);
            }
            return approvalDigest(value);
        }

        [[nodiscard]] std::size_t previewContentBytes(const std::vector<PreviewFile>& files) {
            std::size_t total = 0;
            for (const auto& file : files) {
                if (file.beforeContent.size() > maxPreviewTotalBytes - total) {
                    throw ProjectError(ProjectErrorCode::PreviewTooLarge, "The mutation preview exceeds 8 MiB.");
                }
                total += file.beforeContent.size();
                if (file.afterContent.size() > maxPreviewTotalBytes - total) {
                    throw ProjectError(ProjectErrorCode::PreviewTooLarge, "The mutation preview exceeds 8 MiB.");
                }
                total += file.afterContent.size();
            }
            return total;
        }

        struct ApprovalData {
            std::vector<fs::path>       targetManifestPaths;
            std::vector<UuidAssignment> uuidAssignments;
        };

        [[nodiscard]] std::string createOpaqueApproval(
            const MutationPreview&             preview,
            const std::vector<fs::path>&       targetManifestPaths,
            const std::vector<UuidAssignment>& uuidAssignments
        ) {
            std::string payload;
            appendApprovalString(payload, "mcdk-mutation-preview");
            appendUnsigned(payload, protocolVersion);
            appendApprovalString(payload, previewBinding(preview));
            appendUnsigned(payload, targetManifestPaths.size());
            for (const auto& path : targetManifestPaths) {
                appendApprovalString(payload, pathToUtf8(path.lexically_normal()));
            }
            appendUnsigned(payload, uuidAssignments.size());
            for (const auto& assignment : uuidAssignments) {
                appendApprovalString(payload, pathToUtf8(assignment.manifestPath.lexically_normal()));
                appendUnsigned(payload, assignment.slot);
                appendApprovalString(payload, assignment.before);
                appendApprovalString(payload, assignment.after);
            }
            return hexEncode(payload) + "." + approvalDigest(payload);
        }

        [[nodiscard]] ApprovalData parseOpaqueApproval(const MutationPreview& preview) {
            const auto separator = preview.opaqueApproval.find('.');
            if (separator == std::string::npos
                || preview.opaqueApproval.find('.', separator + 1) != std::string::npos) {
                throw ProjectError(ProjectErrorCode::InvalidPreview, "Malformed opaque preview approval.");
            }
            const auto payload = hexDecode(std::string_view(preview.opaqueApproval).substr(0, separator));
            if (approvalDigest(payload) != std::string_view(preview.opaqueApproval).substr(separator + 1)) {
                throw ProjectError(ProjectErrorCode::InvalidPreview, "The opaque preview approval was modified.");
            }
            ApprovalReader reader(payload);
            if (reader.readString() != "mcdk-mutation-preview" || reader.readUnsigned() != protocolVersion) {
                throw ProjectError(ProjectErrorCode::InvalidPreview, "Unsupported opaque preview approval.");
            }
            if (reader.readString() != previewBinding(preview)) {
                throw ProjectError(
                    ProjectErrorCode::InvalidPreview,
                    "The mutation preview was modified after approval."
                );
            }
            ApprovalData result;
            const auto   targetCount = reader.readUnsigned();
            if (targetCount > 1024 * 1024) {
                throw ProjectError(ProjectErrorCode::InvalidPreview, "Invalid target manifest count in approval.");
            }
            result.targetManifestPaths.reserve(static_cast<std::size_t>(targetCount));
            for (std::uint64_t index = 0; index < targetCount; ++index) {
                result.targetManifestPaths.push_back(fs::u8path(reader.readString()));
            }
            const auto assignmentCount = reader.readUnsigned();
            if (assignmentCount > 1024 * 1024) {
                throw ProjectError(ProjectErrorCode::InvalidPreview, "Invalid UUID assignment count in approval.");
            }
            result.uuidAssignments.reserve(static_cast<std::size_t>(assignmentCount));
            for (std::uint64_t index = 0; index < assignmentCount; ++index) {
                result.uuidAssignments.push_back({
                    fs::u8path(reader.readString()),
                    static_cast<std::size_t>(reader.readUnsigned()),
                    reader.readString(),
                    reader.readString(),
                });
            }
            reader.requireEnd();
            return result;
        }

        [[nodiscard]] MutationPreview makePreview(
            MutationOperation             operation,
            const CanonicalMutationRoots& roots,
            std::optional<VersionPart>    part,
            const MutationPlan&           plan
        ) {
            MutationPreview preview;
            preview.id          = createSecureUuid();
            preview.operation   = operation;
            preview.root        = roots.workspace;
            preview.versionPart = part;
            if (roots.explicitTarget) {
                preview.target = roots.target;
            }
            preview.files.reserve(plan.changes.size());
            for (const auto& change : plan.changes) {
                preview.files.push_back({change.path, change.expectedContent, change.content});
            }
            (void)previewContentBytes(preview.files);
            preview.opaqueApproval = createOpaqueApproval(preview, plan.targetManifestPaths, plan.uuidAssignments);
            return preview;
        }

    } // namespace

    ProjectError::ProjectError(ProjectErrorCode code, std::string message)
    : std::runtime_error(std::move(message)),
      errorCode_(code) {}

    ProjectError::ProjectError(ProjectErrorCode code, fs::path path, std::string message)
    : std::runtime_error(std::move(message)),
      errorCode_(code),
      path_(std::move(path)) {}

    ProjectErrorCode ProjectError::code() const noexcept { return errorCode_; }

    std::string_view ProjectError::codeName() const noexcept { return projectErrorCodeName(errorCode_); }

    const std::optional<fs::path>& ProjectError::path() const noexcept { return path_; }

    std::string_view projectKindName(ProjectKind kind) noexcept {
        switch (kind) {
        case ProjectKind::SinglePack:
            return "single_pack";
        case ProjectKind::Addon:
            return "addon";
        case ProjectKind::Map:
            return "map";
        }
        return "unknown";
    }

    std::string_view packKindName(PackKind kind) noexcept {
        switch (kind) {
        case PackKind::Behavior:
            return "behavior";
        case PackKind::Resource:
            return "resource";
        case PackKind::Unknown:
            return "unknown";
        }
        return "unknown";
    }

    std::string_view projectErrorCodeName(ProjectErrorCode code) noexcept {
        switch (code) {
        case ProjectErrorCode::InvalidProject:
            return "invalid_project";
        case ProjectErrorCode::InvalidManifest:
            return "invalid_manifest";
        case ProjectErrorCode::MissingField:
            return "missing_field";
        case ProjectErrorCode::DuplicateUuid:
            return "duplicate_uuid";
        case ProjectErrorCode::AmbiguousReference:
            return "ambiguous_reference";
        case ProjectErrorCode::VersionOverflow:
            return "version_overflow";
        case ProjectErrorCode::SourceChanged:
            return "source_changed";
        case ProjectErrorCode::Busy:
            return "busy";
        case ProjectErrorCode::IoError:
            return "io_error";
        case ProjectErrorCode::InvalidTarget:
            return "invalid_target";
        case ProjectErrorCode::InvalidPreview:
            return "invalid_preview";
        case ProjectErrorCode::PreviewStale:
            return "preview_stale";
        case ProjectErrorCode::PreviewTooLarge:
            return "preview_too_large";
        case ProjectErrorCode::OutOfScopeReference:
            return "out_of_scope_reference";
        }
        return "unknown_error";
    }

    std::string_view mutationOperationName(MutationOperation operation) noexcept {
        switch (operation) {
        case MutationOperation::BumpVersion:
            return "bump_version";
        case MutationOperation::RegenerateUuids:
            return "regenerate_uuids";
        }
        return "unknown";
    }

    std::size_t ProjectSummary::behaviorPackCount() const noexcept {
        return static_cast<std::size_t>(std::ranges::count_if(manifests, [](const auto& manifest) {
            return manifest.kind == PackKind::Behavior;
        }));
    }

    std::size_t ProjectSummary::resourcePackCount() const noexcept {
        return static_cast<std::size_t>(std::ranges::count_if(manifests, [](const auto& manifest) {
            return manifest.kind == PackKind::Resource;
        }));
    }

    static ProjectSummary inspectProjectUnchecked(const fs::path& root, const std::optional<fs::path>& target) {
        const auto roots = resolveMutationRoots(root, target);
        const auto locks = lockProjectRoots(roots.workspace, roots.target);
        return discoverProject(roots.workspace, roots.explicitTarget ? std::optional{roots.target} : std::nullopt)
            .summary;
    }

    static OperationResult prepareOrApplyMutationUnchecked(
        const fs::path&                root,
        const std::optional<fs::path>& target,
        MutationOperation              operation,
        std::optional<VersionPart>     part,
        bool                           previewOnly
    ) {
        const auto   roots           = resolveMutationRoots(root, target);
        const auto   locks           = lockProjectRoots(roots.workspace, roots.target);
        const auto   canonicalTarget = roots.explicitTarget ? std::optional{roots.target} : std::nullopt;
        auto         loaded          = discoverProject(roots.workspace, canonicalTarget);
        MutationPlan plan;
        switch (operation) {
        case MutationOperation::BumpVersion:
            if (!part) {
                throw ProjectError(ProjectErrorCode::InvalidPreview, "A version bump requires a version part.");
            }
            plan = buildVersionPlan(loaded, *part, roots.explicitTarget);
            break;
        case MutationOperation::RegenerateUuids:
            if (part) {
                throw ProjectError(ProjectErrorCode::InvalidPreview, "UUID regeneration cannot have a version part.");
            }
            plan = buildUuidPlan(loaded, roots.explicitTarget);
            break;
        }
        if (previewOnly) {
            OperationResult result;
            result.project  = loaded.summary;
            result.warnings = loaded.summary.warnings;
            result.preview  = makePreview(operation, roots, part, plan);
            return result;
        }
        commitChanges(plan.changes);
        return resultAfterMutation(roots.workspace, canonicalTarget, std::move(plan.modifiedFiles));
    }

    [[nodiscard]] static std::string readPreviewSource(const fs::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            throw ProjectError(ProjectErrorCode::PreviewStale, path, "A preview source file is no longer readable.");
        }
        std::string result{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        if (input.bad()) {
            throw ProjectError(ProjectErrorCode::PreviewStale, path, "A preview source file could not be read.");
        }
        return result;
    }

    [[nodiscard]] static bool samePathList(const std::vector<fs::path>& left, const std::vector<fs::path>& right) {
        if (left.size() != right.size()) {
            return false;
        }
        for (std::size_t index = 0; index < left.size(); ++index) {
            if (!pathComponentEqual(left[index], right[index])) {
                return false;
            }
        }
        return true;
    }

    static OperationResult applyProjectPreviewUnchecked(const fs::path& root, const MutationPreview& preview) {
        if (preview.id.empty() || preview.files.empty()) {
            throw ProjectError(ProjectErrorCode::InvalidPreview, "The mutation preview is incomplete.");
        }
        if ((preview.operation == MutationOperation::BumpVersion && !preview.versionPart)
            || (preview.operation == MutationOperation::RegenerateUuids && preview.versionPart)) {
            throw ProjectError(ProjectErrorCode::InvalidPreview, "The mutation preview operation is inconsistent.");
        }
        if (preview.operation != MutationOperation::BumpVersion
            && preview.operation != MutationOperation::RegenerateUuids) {
            throw ProjectError(ProjectErrorCode::InvalidPreview, "The mutation preview operation is unknown.");
        }
        if (preview.versionPart && *preview.versionPart != VersionPart::Major
            && *preview.versionPart != VersionPart::Minor && *preview.versionPart != VersionPart::Patch) {
            throw ProjectError(ProjectErrorCode::InvalidPreview, "The mutation preview version part is unknown.");
        }
        const auto approval = parseOpaqueApproval(preview);
        (void)previewContentBytes(preview.files);

        CanonicalMutationRoots roots;
        try {
            roots = resolveMutationRoots(root, preview.target);
        } catch (const ProjectError&) {
            throw ProjectError(ProjectErrorCode::PreviewStale, root, "The approved project roots are no longer valid.");
        }
        if (!pathComponentEqual(roots.workspace, preview.root)) {
            throw ProjectError(
                ProjectErrorCode::InvalidPreview,
                preview.root,
                "The preview belongs to another workspace."
            );
        }
        const auto locks = lockProjectRoots(roots.workspace, roots.target);

        std::unordered_set<std::string> seenPaths;
        for (const auto& file : preview.files) {
            const auto normalized = file.path.lexically_normal();
            if (!file.path.is_absolute() || normalized != file.path
                || (!isPathInside(normalized, roots.workspace) && !isPathInside(normalized, roots.target))) {
                throw ProjectError(
                    ProjectErrorCode::InvalidPreview,
                    file.path,
                    "A preview file is outside approved roots."
                );
            }
            const auto boundary = isPathInside(normalized, roots.workspace) ? roots.workspace : roots.target;
            if (containsReparseComponent(boundary, normalized)) {
                throw ProjectError(
                    ProjectErrorCode::InvalidPreview,
                    file.path,
                    "A preview file crosses a linked path."
                );
            }
#ifdef _WIN32
            const auto key = lowercaseAscii(pathToUtf8(normalized));
#else
            const auto key = pathToUtf8(normalized);
#endif
            if (!seenPaths.insert(key).second) {
                throw ProjectError(ProjectErrorCode::InvalidPreview, file.path, "A preview file is duplicated.");
            }
            if (!isRegularFileWithoutLinks(normalized) || readPreviewSource(normalized) != file.beforeContent) {
                throw ProjectError(ProjectErrorCode::PreviewStale, file.path, "A preview source file changed.");
            }
        }
        std::unordered_set<std::string> approvedTargetPaths;
        for (const auto& path : approval.targetManifestPaths) {
            if (!path.is_absolute() || path.lexically_normal() != path || !isPathInside(path, roots.target)) {
                throw ProjectError(
                    ProjectErrorCode::InvalidPreview,
                    path,
                    "An approved target manifest path is invalid."
                );
            }
#ifdef _WIN32
            const auto key = lowercaseAscii(pathToUtf8(path));
#else
            const auto key = pathToUtf8(path);
#endif
            if (!approvedTargetPaths.insert(key).second) {
                throw ProjectError(
                    ProjectErrorCode::InvalidPreview,
                    path,
                    "An approved target manifest is duplicated."
                );
            }
        }
        std::unordered_set<std::string> approvedAssignmentKeys;
        std::unordered_set<std::string> approvedReplacementUuids;
        for (const auto& assignment : approval.uuidAssignments) {
            if (!assignment.manifestPath.is_absolute()
                || assignment.manifestPath.lexically_normal() != assignment.manifestPath
                || !isPathInside(assignment.manifestPath, roots.target)) {
                throw ProjectError(
                    ProjectErrorCode::InvalidPreview,
                    assignment.manifestPath,
                    "An approved UUID assignment path is invalid."
                );
            }
            const auto key = manifestAssignmentKey(assignment.manifestPath, assignment.slot);
            if (!approvedAssignmentKeys.insert(key).second) {
                throw ProjectError(
                    ProjectErrorCode::InvalidPreview,
                    assignment.manifestPath,
                    "An approved UUID assignment is duplicated."
                );
            }
            try {
                JsonNode beforeNode;
                beforeNode.type        = JsonType::String;
                beforeNode.stringValue = assignment.before;
                JsonNode afterNode;
                afterNode.type        = JsonType::String;
                afterNode.stringValue = assignment.after;
                (void)normalizeUuid(beforeNode, assignment.manifestPath);
                const auto replacement = normalizeUuid(afterNode, assignment.manifestPath);
                if (!approvedReplacementUuids.insert(replacement).second) {
                    throw ProjectError(
                        ProjectErrorCode::InvalidPreview,
                        assignment.manifestPath,
                        "An approved replacement UUID is duplicated."
                    );
                }
            } catch (const ProjectError& error) {
                if (error.code() == ProjectErrorCode::InvalidPreview) {
                    throw;
                }
                throw ProjectError(
                    ProjectErrorCode::InvalidPreview,
                    assignment.manifestPath,
                    "An approved UUID assignment is malformed."
                );
            }
        }

        const auto    canonicalTarget = roots.explicitTarget ? std::optional{roots.target} : std::nullopt;
        LoadedProject loaded;
        MutationPlan  plan;
        try {
            loaded = discoverProject(roots.workspace, canonicalTarget);
            if (preview.operation == MutationOperation::BumpVersion) {
                plan = buildVersionPlan(loaded, *preview.versionPart, roots.explicitTarget);
            } else {
                plan = buildUuidPlan(loaded, roots.explicitTarget, &approval.uuidAssignments);
            }
        } catch (const ProjectError& error) {
            throw ProjectError(ProjectErrorCode::PreviewStale, error.path().value_or(root), error.what());
        }
        if (!samePathList(plan.targetManifestPaths, approval.targetManifestPaths)
            || plan.changes.size() != preview.files.size()) {
            throw ProjectError(ProjectErrorCode::PreviewStale, "The selected target or affected file set changed.");
        }
        for (std::size_t index = 0; index < plan.changes.size(); ++index) {
            const auto& change = plan.changes[index];
            const auto& file   = preview.files[index];
            if (!pathComponentEqual(change.path, file.path) || change.expectedContent != file.beforeContent
                || change.content != file.afterContent) {
                throw ProjectError(ProjectErrorCode::PreviewStale, file.path, "The approved mutation diff is stale.");
            }
        }
        try {
            commitChanges(plan.changes);
        } catch (const ProjectError& error) {
            if (error.code() != ProjectErrorCode::SourceChanged) {
                throw;
            }
            throw ProjectError(ProjectErrorCode::PreviewStale, error.path().value_or(root), error.what());
        }
        return resultAfterMutation(roots.workspace, canonicalTarget, std::move(plan.modifiedFiles));
    }

    ProjectSummary inspectProject(const fs::path& root) { return inspectProject(root, std::nullopt); }

    ProjectSummary inspectProject(const fs::path& root, const std::optional<fs::path>& target) {
        try {
            return inspectProjectUnchecked(root, target);
        } catch (const ProjectError&) {
            throw;
        } catch (const fs::filesystem_error& error) {
            throw ProjectError(ProjectErrorCode::IoError, error.path1(), error.what());
        } catch (const std::exception& error) {
            throw ProjectError(ProjectErrorCode::IoError, root, error.what());
        }
    }

    OperationResult regenerateProjectUuids(const fs::path& root) {
        return regenerateProjectUuids(root, std::nullopt, false);
    }

    OperationResult regenerateProjectUuids(const fs::path& root, const std::optional<fs::path>& target, bool preview) {
        try {
            return prepareOrApplyMutationUnchecked(
                root,
                target,
                MutationOperation::RegenerateUuids,
                std::nullopt,
                preview
            );
        } catch (const ProjectError&) {
            throw;
        } catch (const fs::filesystem_error& error) {
            throw ProjectError(ProjectErrorCode::IoError, error.path1(), error.what());
        } catch (const std::exception& error) {
            throw ProjectError(ProjectErrorCode::IoError, root, error.what());
        }
    }

    OperationResult bumpProjectVersion(const fs::path& root, VersionPart part) {
        return bumpProjectVersion(root, part, std::nullopt, false);
    }

    OperationResult
    bumpProjectVersion(const fs::path& root, VersionPart part, const std::optional<fs::path>& target, bool preview) {
        try {
            return prepareOrApplyMutationUnchecked(root, target, MutationOperation::BumpVersion, part, preview);
        } catch (const ProjectError&) {
            throw;
        } catch (const fs::filesystem_error& error) {
            throw ProjectError(ProjectErrorCode::IoError, error.path1(), error.what());
        } catch (const std::exception& error) {
            throw ProjectError(ProjectErrorCode::IoError, root, error.what());
        }
    }

    OperationResult applyProjectPreview(const fs::path& root, const MutationPreview& preview) {
        try {
            return applyProjectPreviewUnchecked(root, preview);
        } catch (const ProjectError&) {
            throw;
        } catch (const fs::filesystem_error& error) {
            throw ProjectError(ProjectErrorCode::IoError, error.path1(), error.what());
        } catch (const std::exception& error) {
            throw ProjectError(ProjectErrorCode::IoError, root, error.what());
        }
    }

} // namespace mcdk::project
