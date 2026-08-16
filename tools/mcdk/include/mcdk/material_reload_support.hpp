#pragma once

#include <string>
#include <vector>

namespace mcdk::material_reload_support {

    [[nodiscard]] std::string
    buildReloadMaterialsPythonCode(const std::vector<std::string>& materialPaths, bool checkSyntax);

} // namespace mcdk::material_reload_support
