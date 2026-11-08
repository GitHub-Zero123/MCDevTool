#include <mcdk/log_query.hpp>

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>

namespace mcdk::log_query {
    namespace {
        using Json = nlohmann::json;

        Json textResult(bool isError, std::string text) {
            return {{"isError", isError}, {"content", Json::array({{{"type", "text"}, {"text", std::move(text)}}})}};
        }

        std::string lowerAscii(std::string_view text) {
            std::string result(text);
            std::ranges::transform(result, result.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return result;
        }

        std::function<bool(std::string_view)> buildMatcher(const Json& params) {
            const bool  ignoreCase = params.value("ignore_case", false);
            std::string contains   = params.value("contains", std::string{});
            std::string pattern    = params.value("regex", std::string{});
            if (contains.empty() && pattern.empty()) {
                return {};
            }
            std::optional<std::regex> regex;
            if (!pattern.empty()) {
                auto flags = std::regex::ECMAScript | std::regex::optimize;
                if (ignoreCase) {
                    flags |= std::regex::icase;
                }
                regex.emplace(pattern, flags); // 非法表达式抛 std::regex_error
            }
            if (ignoreCase) {
                contains = lowerAscii(contains);
            }
            return [contains, regex, ignoreCase](std::string_view line) {
                if (!contains.empty()) {
                    const auto haystack = ignoreCase ? lowerAscii(line) : std::string(line);
                    if (haystack.find(contains) == std::string::npos) {
                        return false;
                    }
                }
                return !regex || std::regex_search(line.begin(), line.end(), *regex);
            };
        }
    } // namespace

    Json readLogs(const LogBuffer& logs, const LogBuffer& errors, const Json& params) {
        LogBuffer::Query query;
        try {
            query.match = buildMatcher(params);
        } catch (const std::regex_error& error) {
            return textResult(true, std::string("Invalid regex: ") + error.what());
        }
        if (const auto since = params.find("since"); since != params.end() && since->is_number()) {
            query.since = since->get<std::uint64_t>();
        }
        const auto limit = params.value("limit", static_cast<std::int64_t>(DefaultLimit));
        query.limit      = static_cast<std::size_t>(std::clamp<std::int64_t>(limit, 1, MaxLimit));

        const auto stream = params.value("stream", std::string("all"));
        if (stream != "all" && stream != "errors") {
            return textResult(true, "stream must be \"all\" or \"errors\"");
        }
        const auto page = (stream == "errors" ? errors : logs).query(query);

        if (params.value("format", std::string("text")) == "json") {
            auto lines = Json::array();
            for (const auto& entry : page.entries) {
                lines.push_back({{"seq", entry.seq}, {"text", entry.text}});
            }
            const Json body{{"lines", std::move(lines)}, {"next", page.next}, {"skipped", page.skipped}, {"more", page.more}};
            return textResult(false, body.dump());
        }

        std::string text;
        for (const auto& entry : page.entries) {
            text += entry.text;
            text += '\n';
        }
        text += "[logs] next=" + std::to_string(page.next);
        if (page.skipped > 0) {
            text += " skipped=" + std::to_string(page.skipped);
        }
        if (page.more) {
            text += " more";
        }
        return textResult(false, std::move(text));
    }

} // namespace mcdk::log_query
