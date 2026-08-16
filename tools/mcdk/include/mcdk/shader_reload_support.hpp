#pragma once

#include <string>
#include <vector>

namespace mcdk::shader_reload_support {

    [[nodiscard]] std::string
    buildReloadShadersPythonCode(const std::vector<std::string>& shaderNames, bool checkSyntax);

} // namespace mcdk::shader_reload_support
