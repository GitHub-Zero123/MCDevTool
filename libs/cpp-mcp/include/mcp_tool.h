/**
 * @file mcp_tool.h
 * @brief Tool definitions and helper functions for MCP
 *
 * This file provides tool-related functionality and abstractions for the MCP protocol.
 * Supports 2025-03-26 tool annotations and structured output.
 */

#ifndef MCP_TOOL_H
#define MCP_TOOL_H

#include "mcp_message.h"
#include <functional>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <optional>

namespace mcp {

    // Tool annotations (2025-03-26)
    struct tool_annotations {
        std::optional<std::string> title;            // Human-readable title
        std::optional<bool>        read_only_hint;   // readOnlyHint: tool doesn't modify environment
        std::optional<bool>        destructive_hint; // destructiveHint: may perform destructive operations
        std::optional<bool>        idempotent_hint;  // idempotentHint: same args always same result
        std::optional<bool>        open_world_hint;  // openWorldHint: interacts with external entities

        json to_json() const {
            json j = json::object();
            if (title.has_value()) j["title"] = title.value();
            if (read_only_hint.has_value()) j["readOnlyHint"] = read_only_hint.value();
            if (destructive_hint.has_value()) j["destructiveHint"] = destructive_hint.value();
            if (idempotent_hint.has_value()) j["idempotentHint"] = idempotent_hint.value();
            if (open_world_hint.has_value()) j["openWorldHint"] = open_world_hint.value();
            return j;
        }

        bool empty() const {
            return !title.has_value() && !read_only_hint.has_value() && !destructive_hint.has_value()
                && !idempotent_hint.has_value() && !open_world_hint.has_value();
        }
    };

    // MCP Tool definition
    struct tool {
        std::string      name;
        std::string      description;
        json             parameters_schema;
        json             output_schema; // Structured output schema (2025-03-26)
        tool_annotations annotations;   // Tool annotations (2025-03-26)

        // Convert to JSON for API documentation
        json to_json() const {
            json j = {{"name", name}, {"description", description}, {"inputSchema", parameters_schema}};
            if (!output_schema.is_null() && !output_schema.empty()) {
                j["outputSchema"] = output_schema;
            }
            if (!annotations.empty()) {
                j["annotations"] = annotations.to_json();
            }
            return j;
        }
    };

    /**
     * @class tool_builder
     * @brief Utility class for building tools with a fluent API
     *
     * The tool_builder class provides a simple way to create tools with
     * a fluent (chain-based) API.
     */
    class tool_builder {
    public:
        /**
         * @brief Constructor
         * @param name The name of the tool
         */
        explicit tool_builder(const std::string& name);

        /**
         * @brief Set the tool description
         * @param description The description
         * @return Reference to this builder
         */
        tool_builder& with_description(const std::string& description);

        /**
         * @brief Add a string parameter
         * @param name The parameter name
         * @param description The parameter description
         * @param required Whether the parameter is required
         * @return Reference to this builder
         */
        tool_builder& with_string_param(const std::string& name, const std::string& description, bool required = true);

        /**
         * @brief Add a number parameter
         * @param name The parameter name
         * @param description The parameter description
         * @param required Whether the parameter is required
         * @return Reference to this builder
         */
        tool_builder& with_number_param(const std::string& name, const std::string& description, bool required = true);

        /**
         * @brief Add a boolean parameter
         * @param name The parameter name
         * @param description The parameter description
         * @param required Whether the parameter is required
         * @return Reference to this builder
         */
        tool_builder& with_boolean_param(const std::string& name, const std::string& description, bool required = true);

        /**
         * @brief Add an array parameter
         * @param name The parameter name
         * @param description The parameter description
         * @param item_type The type of the array items ("string", "number", "object", etc.)
         * @param required Whether the parameter is required
         * @return Reference to this builder
         */
        tool_builder& with_array_param(
            const std::string& name,
            const std::string& description,
            const std::string& item_type,
            bool               required = true
        );

        /**
         * @brief Add an object parameter
         * @param name The parameter name
         * @param description The parameter description
         * @param properties JSON schema for the object properties
         * @param required Whether the parameter is required
         * @return Reference to this builder
         */
        tool_builder& with_object_param(
            const std::string& name,
            const std::string& description,
            const json&        properties,
            bool               required = true
        );

        /**
         * @brief Set tool annotations (2025-03-26)
         */
        tool_builder& with_annotations(const tool_annotations& annotations);

        /**
         * @brief Mark tool as read-only (2025-03-26)
         */
        tool_builder& with_read_only_hint(bool hint = true);

        /**
         * @brief Mark tool as potentially destructive (2025-03-26)
         */
        tool_builder& with_destructive_hint(bool hint = true);

        /**
         * @brief Mark tool as idempotent (2025-03-26)
         */
        tool_builder& with_idempotent_hint(bool hint = true);

        /**
         * @brief Mark tool as open-world (2025-03-26)
         */
        tool_builder& with_open_world_hint(bool hint = true);

        /**
         * @brief Set tool title for display (2025-03-26)
         */
        tool_builder& with_title(const std::string& title);

        /**
         * @brief Set structured output schema (2025-03-26)
         */
        tool_builder& with_output_schema(const json& schema);

        /**
         * @brief Build the tool
         * @return The constructed tool
         */
        tool build() const;

    private:
        std::string              name_;
        std::string              description_;
        json                     parameters_;
        std::vector<std::string> required_params_;
        tool_annotations         annotations_;   // Tool annotations (2025-03-26)
        json                     output_schema_; // Output schema (2025-03-26)

        // Helper to add a parameter of any type
        tool_builder&
        add_param(const std::string& name, const std::string& description, const std::string& type, bool required);
    };

    /**
     * @brief Create a simple tool with a function-based approach
     * @param name Tool name
     * @param description Tool description
     * @param handler Function to handle tool invocations
     * @param parameter_definitions A vector of parameter definitions as {name, description, type, required}
     * @return The created tool
     */
    inline tool create_tool(
        const std::string&                                                          name,
        const std::string&                                                          description,
        const std::vector<std::tuple<std::string, std::string, std::string, bool>>& parameter_definitions
    ) {

        tool_builder builder(name);
        builder.with_description(description);

        for (const auto& [param_name, param_desc, param_type, required] : parameter_definitions) {
            if (param_type == "string") {
                builder.with_string_param(param_name, param_desc, required);
            } else if (param_type == "number") {
                builder.with_number_param(param_name, param_desc, required);
            } else if (param_type == "boolean") {
                builder.with_boolean_param(param_name, param_desc, required);
            } else if (param_type == "array") {
                builder.with_array_param(param_name, param_desc, "string", required);
            } else if (param_type == "object") {
                builder.with_object_param(param_name, param_desc, json::object(), required);
            }
        }

        return builder.build();
    }

} // namespace mcp

#endif // MCP_TOOL_H