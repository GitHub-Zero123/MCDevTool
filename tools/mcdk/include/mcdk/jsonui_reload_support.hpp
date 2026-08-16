#pragma once

#include <string>

namespace mcdk::jsonui_reload_support {

    [[nodiscard]] std::string buildPreparePreserveModUiPythonCode();
    [[nodiscard]] std::string buildRestorePreservedModUiPythonCode();

} // namespace mcdk::jsonui_reload_support
