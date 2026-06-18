#pragma once

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <mcdevtool/utils.h>
#include <nlohmann/json.hpp>

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

    namespace detail {

        struct ValidationSax : nlohmann::json_sax<nlohmann::json> {
            std::size_t position = 0;
            std::string lastToken;
            std::string message;

            bool null() override { return true; }
            bool boolean(bool) override { return true; }
            bool number_integer(number_integer_t) override { return true; }
            bool number_unsigned(number_unsigned_t) override { return true; }
            bool number_float(number_float_t, const string_t&) override { return true; }
            bool string(string_t&) override { return true; }
            bool binary(binary_t&) override { return true; }
            bool start_object(std::size_t) override { return true; }
            bool key(string_t&) override { return true; }
            bool end_object() override { return true; }
            bool start_array(std::size_t) override { return true; }
            bool end_array() override { return true; }
            bool parse_error(
                std::size_t                        position_,
                const std::string&                 lastToken_,
                const nlohmann::detail::exception& ex
            ) override {
                position  = position_;
                lastToken = lastToken_;
                message   = ex.what();
                return false;
            }
        };

        inline std::string cleanParseMessage(std::string message) {
            const auto bracketEnd = message.find("] ");
            if (bracketEnd != std::string::npos) {
                message.erase(0, bracketEnd + 2);
            }

            const std::string columnMarker = "column ";
            const auto        columnPos    = message.find(columnMarker);
            if (columnPos != std::string::npos) {
                const auto detailPos = message.find(": ", columnPos);
                if (detailPos != std::string::npos) {
                    message.erase(0, detailPos + 2);
                }
            }

            const std::string lastReadMarker = "; last read:";
            const auto        lastReadPos    = message.find(lastReadMarker);
            if (lastReadPos != std::string::npos) {
                const auto expectedMarker = message.find("; expected ", lastReadPos + lastReadMarker.size());
                if (expectedMarker != std::string::npos) {
                    message.erase(lastReadPos, expectedMarker - lastReadPos);
                } else {
                    message.erase(lastReadPos);
                }
            }

            return message.empty() ? "syntax error" : message;
        }

        inline void locateBytePosition(
            const std::string& text,
            std::size_t        bytePosition,
            std::size_t&       line,
            std::size_t&       column,
            std::string&       lineText
        ) {
            const std::size_t index = bytePosition > 0 ? bytePosition - 1 : 0;
            line                    = 1;
            column                  = 1;
            std::size_t lineStart   = 0;

            const std::size_t end = index < text.size() ? index : text.size();
            for (std::size_t i = 0; i < end; ++i) {
                if (text[i] == '\n') {
                    ++line;
                    column    = 1;
                    lineStart = i + 1;
                } else {
                    ++column;
                }
            }

            std::size_t lineEnd = text.find('\n', lineStart);
            if (lineEnd == std::string::npos) {
                lineEnd = text.size();
            }
            if (lineEnd > lineStart && text[lineEnd - 1] == '\r') {
                --lineEnd;
            }
            lineText = text.substr(lineStart, lineEnd - lineStart);
        }

        inline std::string makeCaretLine(const std::string& lineText, std::size_t column) {
            std::string       out;
            const std::size_t caretColumn = column > 0 ? column : 1;
            for (std::size_t i = 1; i < caretColumn; ++i) {
                if (i - 1 < lineText.size() && lineText[i - 1] == '\t') {
                    out.push_back('\t');
                } else {
                    out.push_back(' ');
                }
            }
            out.push_back('^');
            return out;
        }

        inline bool isAsciiPrintableToken(const std::string& token) {
            if (token.empty()) {
                return false;
            }
            for (unsigned char ch : token) {
                if (ch == '\t' || ch == '\r' || ch == '\n') {
                    continue;
                }
                if (ch < 0x20 || ch >= 0x7F) {
                    return false;
                }
            }
            return true;
        }

        inline std::string formatDiagnostic(const JsonDiagnostic& diagnostic, const std::string& title) {
            std::ostringstream out;
            out << title << "\n"
                << diagnostic.path << ":" << diagnostic.line << ":" << diagnostic.column << ": "
                << diagnostic.message << "\n"
                << "  " << diagnostic.line << " | " << diagnostic.lineText << "\n"
                << "     | " << diagnostic.caretLine;
            if (isAsciiPrintableToken(diagnostic.lastToken)) {
                out << "\n     = near token: " << diagnostic.lastToken;
            }
            return out.str();
        }

    } // namespace detail

    inline JsonDiagnostic validateJsonTextWithComments(
        const std::string& text,
        std::string        path,
        std::string        title = "warning: invalid JSON"
    ) {
        JsonDiagnostic diagnostic;
        diagnostic.path     = std::move(path);
        diagnostic.readable = true;

        detail::ValidationSax sax;
        if (nlohmann::json::sax_parse(text, &sax, nlohmann::json::input_format_t::json, true, true)) {
            diagnostic.ok = true;
            return diagnostic;
        }

        diagnostic.ok        = false;
        diagnostic.byte      = sax.position;
        diagnostic.lastToken = sax.lastToken;
        diagnostic.message   = detail::cleanParseMessage(sax.message);
        diagnostic.empty     = text.empty();
        detail::locateBytePosition(text, sax.position, diagnostic.line, diagnostic.column, diagnostic.lineText);
        diagnostic.caretLine = detail::makeCaretLine(diagnostic.lineText, diagnostic.column);
        diagnostic.formatted = detail::formatDiagnostic(diagnostic, title);
        return diagnostic;
    }

    inline JsonDiagnostic validateJsonFileWithComments(
        const std::filesystem::path& filePath,
        std::string                  title = "warning: invalid JSON"
    ) {
        JsonDiagnostic diagnostic;
        diagnostic.path = MCDevTool::Utils::pathToGenericUtf8(filePath);

        std::ifstream file(filePath, std::ios::binary);
        if (!file) {
            diagnostic.ok        = false;
            diagnostic.readable  = false;
            diagnostic.message   = "failed to open file";
            diagnostic.formatted = title + "\n" + diagnostic.path + ": " + diagnostic.message;
            return diagnostic;
        }

        std::ostringstream buffer;
        buffer << file.rdbuf();
        return validateJsonTextWithComments(buffer.str(), std::move(diagnostic.path), std::move(title));
    }

} // namespace mcdk::json_diagnostics
