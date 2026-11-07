#include <mcdk/mcp_tool_help.hpp>

#include <algorithm>
#include <cstddef>
#include <sstream>
#include <string_view>

namespace mcdk::mcp_tool_help {
    namespace {
        using Json = mcp::json;

        std::string_view firstLine(std::string_view text) { return text.substr(0, text.find('\n')); }

        std::string typeName(const Json& schema) {
            const auto type = schema.find("type");
            if (type == schema.end()) {
                return "any";
            }
            if (type->is_string()) {
                return type->get<std::string>();
            }
            std::string joined;
            for (const auto& item : *type) {
                joined += (joined.empty() ? "" : "|") + item.get<std::string>();
            }
            return joined;
        }

        bool isRequired(const Json& schema, const std::string& name) {
            const auto required = schema.find("required");
            return required != schema.end() && required->is_array()
                && std::find(required->begin(), required->end(), name) != required->end();
        }

        void writeProperties(std::ostringstream& output, const Json& schema, std::size_t depth) {
            const auto properties = schema.find("properties");
            if (properties == schema.end() || !properties->is_object()) {
                return;
            }
            const std::string indent(depth * 2, ' ');
            for (const auto& [name, property] : properties->items()) {
                output << indent << name << " (" << typeName(property);
                if (isRequired(schema, name)) {
                    output << ", 必填";
                }
                if (const auto minimum = property.find("minimum"); minimum != property.end()) {
                    output << ", 最小 " << minimum->dump();
                }
                if (const auto maximum = property.find("maximum"); maximum != property.end()) {
                    output << ", 最大 " << maximum->dump();
                }
                if (const auto values = property.find("enum"); values != property.end()) {
                    output << ", 可选 " << values->dump();
                }
                output << ")";
                if (const auto description = property.find("description");
                    description != property.end() && description->is_string()) {
                    output << "  " << description->get<std::string>();
                }
                output << '\n';
                writeProperties(output, property, depth + 1);
            }
        }

        Json exampleValue(const Json& property) {
            const auto type = typeName(property);
            if (type == "boolean") {
                return true;
            }
            if (type == "number" || type == "integer") {
                return 1;
            }
            if (type == "object") {
                return Json::object();
            }
            if (type == "array") {
                return Json::array();
            }
            return "...";
        }

        std::string exampleArguments(const mcp::tool& tool) {
            auto       arguments = Json::object();
            const auto required  = tool.parameters_schema.find("required");
            const auto schema    = tool.parameters_schema.find("properties");
            if (required != tool.parameters_schema.end() && schema != tool.parameters_schema.end()) {
                for (const auto& name : *required) {
                    const auto key = name.get<std::string>();
                    if (const auto property = schema->find(key); property != schema->end()) {
                        arguments[key] = exampleValue(*property);
                    }
                }
            }
            return arguments.dump();
        }
    } // namespace

    std::string formatToolList(const std::vector<mcp::tool>& tools) {
        std::size_t width = 0;
        for (const auto& tool : tools) {
            width = std::max(width, tool.name.size());
        }
        std::ostringstream output;
        for (const auto& tool : tools) {
            output << "  " << tool.name << std::string(width - tool.name.size() + 2, ' ')
                   << firstLine(tool.description) << '\n';
        }
        return output.str();
    }

    std::string formatToolHelp(const mcp::tool& tool) {
        std::ostringstream output;
        output << tool.name << "\n\n" << tool.description << "\n\n参数：\n";
        if (tool.parameters_schema.contains("properties") && !tool.parameters_schema["properties"].empty()) {
            writeProperties(output, tool.parameters_schema, 1);
        } else {
            output << "  无\n";
        }
        output << "\n调用：\n  mcdk call " << tool.name << " --args '" << exampleArguments(tool) << "'\n";
        return output.str();
    }

} // namespace mcdk::mcp_tool_help
