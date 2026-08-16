#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace mcdk::json_diagnostics {

    struct JsonDiagnostic {
        bool        ok       = false;
        bool        readable = false;
        bool        empty    = false;
        std::size_t line     = 1;
        std::size_t column   = 1;
        std::size_t byte     = 0;
        std::string path;
        std::string message;
        std::string lineText;
        std::string caretLine;
        std::string lastToken;
        std::string formatted;
    };

    [[nodiscard]] JsonDiagnostic validateJsonTextWithComments(
        const std::string& text,
        std::string        path,
        std::string        title = "warning: invalid JSON"
    );

    [[nodiscard]] JsonDiagnostic
    validateJsonFileWithComments(const std::filesystem::path& filePath, std::string title = "warning: invalid JSON");

} // namespace mcdk::json_diagnostics
