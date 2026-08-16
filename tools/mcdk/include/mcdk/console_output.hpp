#pragma once

#include <string>

#include <mcdk/console.hpp>

namespace mcdk {

    void printColoredAtomic(const std::string& message, ConsoleColor color);
    void printStartupLogo(bool pluginEnvironment);

} // namespace mcdk
