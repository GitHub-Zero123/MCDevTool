#pragma once

#include <string>

#include "console.hpp"

namespace mcdk {

    void printColoredAtomic(const std::string& message, ConsoleColor color);
    void printStartupLogo(bool pluginEnvironment);

} // namespace mcdk
