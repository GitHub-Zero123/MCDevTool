#include <json_diagnostics.hpp>

#include <fstream>
#include <sstream>
#include <utility>

#include <mcdevtool/utils.h>
#include <nlohmann/json.hpp>

namespace mcdk::json_diagnostics {
    namespace {
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
                std::size_t                        errorPosition,
                const std::string&                 errorToken,
                const nlohmann::detail::exception& exception
            ) override {
                position  = errorPosition;
                lastToken = errorToken;
                message   = exception.what();
                return false;
            }
        };

        std::string cleanParseMessage(std::string message) {
            if (const auto bracketEnd = message.find("] "); bracketEnd != std::string::npos) {
                message.erase(0, bracketEnd + 2);
            }
            if (const auto columnPosition = message.find("column "); columnPosition != std::string::npos) {
                if (const auto detailPosition = message.find(": ", columnPosition);
                    detailPosition != std::string::npos) {
                    message.erase(0, detailPosition + 2);
                }
            }
            constexpr std::string_view lastReadMarker = "; last read:";
            if (const auto lastReadPosition = message.find(lastReadMarker); lastReadPosition != std::string::npos) {
                const auto expectedPosition = message.find("; expected ", lastReadPosition + lastReadMarker.size());
                if (expectedPosition != std::string::npos) {
                    message.erase(lastReadPosition, expectedPosition - lastReadPosition);
                } else {
                    message.erase(lastReadPosition);
                }
            }
            return message.empty() ? "syntax error" : message;
        }

        void locateBytePosition(
            const std::string& text,
            std::size_t        bytePosition,
            std::size_t&       line,
            std::size_t&       column,
            std::string&       lineText
        ) {
            const std::size_t index     = bytePosition > 0 ? bytePosition - 1 : 0;
            line                        = 1;
            column                      = 1;
            std::size_t       lineStart = 0;
            const std::size_t end       = std::min(index, text.size());
            for (std::size_t position = 0; position < end; ++position) {
                if (text[position] == '\n') {
                    ++line;
                    column    = 1;
                    lineStart = position + 1;
                } else {
                    ++column;
                }
            }

            auto lineEnd = text.find('\n', lineStart);
            if (lineEnd == std::string::npos) {
                lineEnd = text.size();
            }
            if (lineEnd > lineStart && text[lineEnd - 1] == '\r') {
                --lineEnd;
            }
            lineText = text.substr(lineStart, lineEnd - lineStart);
        }

        std::string makeCaretLine(const std::string& lineText, std::size_t column) {
            std::string result;
            const auto  caretColumn = std::max<std::size_t>(column, 1);
            for (std::size_t index = 1; index < caretColumn; ++index) {
                result.push_back(index - 1 < lineText.size() && lineText[index - 1] == '\t' ? '\t' : ' ');
            }
            result.push_back('^');
            return result;
        }

        bool isAsciiPrintableToken(const std::string& token) {
            if (token.empty()) {
                return false;
            }
            for (const unsigned char character : token) {
                if (character == '\t' || character == '\r' || character == '\n') {
                    continue;
                }
                if (character < 0x20 || character >= 0x7F) {
                    return false;
                }
            }
            return true;
        }

        std::string formatDiagnostic(const JsonDiagnostic& diagnostic, const std::string& title) {
            std::ostringstream output;
            output << title << '\n'
                   << diagnostic.path << ':' << diagnostic.line << ':' << diagnostic.column << ": "
                   << diagnostic.message << '\n'
                   << "  " << diagnostic.line << " | " << diagnostic.lineText << '\n'
                   << "     | " << diagnostic.caretLine;
            if (isAsciiPrintableToken(diagnostic.lastToken)) {
                output << "\n     = near token: " << diagnostic.lastToken;
            }
            return output.str();
        }
    } // namespace

    JsonDiagnostic validateJsonTextWithComments(const std::string& text, std::string path, std::string title) {
        JsonDiagnostic diagnostic;
        diagnostic.path     = std::move(path);
        diagnostic.readable = true;

        ValidationSax sax;
        if (nlohmann::json::sax_parse(text, &sax, nlohmann::json::input_format_t::json, true, true)) {
            diagnostic.ok = true;
            return diagnostic;
        }

        diagnostic.byte      = sax.position;
        diagnostic.lastToken = sax.lastToken;
        diagnostic.message   = cleanParseMessage(sax.message);
        diagnostic.empty     = text.empty();
        locateBytePosition(text, sax.position, diagnostic.line, diagnostic.column, diagnostic.lineText);
        diagnostic.caretLine = makeCaretLine(diagnostic.lineText, diagnostic.column);
        diagnostic.formatted = formatDiagnostic(diagnostic, title);
        return diagnostic;
    }

    JsonDiagnostic validateJsonFileWithComments(const std::filesystem::path& filePath, std::string title) {
        JsonDiagnostic diagnostic;
        diagnostic.path = MCDevTool::Utils::pathToGenericUtf8(filePath);

        std::ifstream file(filePath, std::ios::binary);
        if (!file) {
            diagnostic.message   = "failed to open file";
            diagnostic.formatted = title + '\n' + diagnostic.path + ": " + diagnostic.message;
            return diagnostic;
        }

        std::ostringstream buffer;
        buffer << file.rdbuf();
        return validateJsonTextWithComments(buffer.str(), std::move(diagnostic.path), std::move(title));
    }

} // namespace mcdk::json_diagnostics
