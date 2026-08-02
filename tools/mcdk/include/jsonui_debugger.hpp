#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

namespace mcdk::jsonui_debugger {

    inline constexpr const char* ToolName = "jsonui_debugger";
    inline constexpr const char* ToolDescription =
        "Analyze native Minecraft JSON UI runtime state through a command string. "
        "Pass the command in the cmd argument; use cmd=\"/help\" to list available commands and usage. "
        "Supports screen listing, node lookup, shallow tree inspection, layout snapshots, and HTML-like pseudo output "
        "derived from Minecraft runtime layout/render data for layout reference only. "
        "Read-only by default with depth/node limits to avoid large UI dumps.";

    [[nodiscard]] std::string                trimCopy(std::string_view value);
    [[nodiscard]] bool                       startsWith(std::string_view value, std::string_view prefix);
    [[nodiscard]] bool                       commandHasFlag(std::string_view command, std::string_view flag);
    [[nodiscard]] std::optional<std::string> commandOptionValue(std::string_view command, std::string_view optionName);

    [[nodiscard]] nlohmann::json buildLocalHelpJson(std::string_view command);
    [[nodiscard]] std::string    buildPythonCode(std::string_view command);
    [[nodiscard]] nlohmann::json parseFirstJsonFromDirtyText(std::string_view text);

    void attachHtmlPseudoIfRequested(std::string_view command, nlohmann::json& parsed);
    void attachSvgDiagramIfRequested(std::string_view command, nlohmann::json& parsed);

} // namespace mcdk::jsonui_debugger
