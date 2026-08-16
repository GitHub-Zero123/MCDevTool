#include <mcdk/console_output.hpp>

#include <iostream>
#include <mutex>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define _MCDEV_LOG_OUTPUT_ENDL "\n"

#ifdef MCDEV_LOG_FORCE_FLUSH_ENDL
#undef _MCDEV_LOG_OUTPUT_ENDL
#define _MCDEV_LOG_OUTPUT_ENDL std::endl
#endif

namespace {
    std::mutex gConsoleMutex;
}

void mcdk::printColoredAtomic(const std::string& message, ConsoleColor color) {
    std::lock_guard<std::mutex> lock(gConsoleMutex);
    HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);

    if (console == INVALID_HANDLE_VALUE) {
        std::cout << message << _MCDEV_LOG_OUTPUT_ENDL;
        return;
    }

    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(console, &info)) {
        std::cout << message << _MCDEV_LOG_OUTPUT_ENDL;
        return;
    }

    WORD attributes = 0;
    switch (color) {
    case ConsoleColor::Green:
        attributes = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        break;
    case ConsoleColor::Red:
        attributes = FOREGROUND_RED | FOREGROUND_INTENSITY;
        break;
    case ConsoleColor::Blue:
        attributes = FOREGROUND_BLUE | FOREGROUND_INTENSITY;
        break;
    case ConsoleColor::Yellow:
        attributes = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        break;
    case ConsoleColor::Cyan:
        attributes = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
        break;
    case ConsoleColor::Magenta:
        attributes = FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
        break;
    case ConsoleColor::White:
        attributes = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
        break;
    case ConsoleColor::Black:
        attributes = 0;
        break;
    case ConsoleColor::Gray:
        attributes = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
        break;
    case ConsoleColor::DarkGray:
        attributes = FOREGROUND_INTENSITY;
        break;
    default:
        break;
    }

    if (color != ConsoleColor::Default) {
        SetConsoleTextAttribute(console, attributes);
    }
    std::cout << message << _MCDEV_LOG_OUTPUT_ENDL;
    if (color == ConsoleColor::Default) {
        return;
    }
    SetConsoleTextAttribute(console, info.wAttributes);
}

void mcdk::printStartupLogo(bool pluginEnvironment) {
    std::cout << _MCDEV_LOG_OUTPUT_ENDL;
    std::cout << "  ███╗   ███╗ ██████╗ ██████╗ ██╗  ██╗\n"
              << "  ████╗ ████║██╔════╝ ██╔══██╗██║ ██╔╝\n"
              << "  ██╔████╔██║██║      ██║  ██║█████╔╝\n"
              << "  ██║╚██╔╝██║██║      ██║  ██║██╔═██╗\n"
              << "  ██║ ╚═╝ ██║╚██████╗ ██████╔╝██║  ██╗\n"
              << "  ╚═╝     ╚═╝ ╚═════╝ ╚═════╝ ╚═╝  ╚═╝" << _MCDEV_LOG_OUTPUT_ENDL;
    printColoredAtomic("  Minecraft Creator Development Kit", ConsoleColor::DarkGray);
    if (pluginEnvironment) {
        printColoredAtomic("  Kid Studio Core Tool · VSCode Extension: Dofes, Zero123", ConsoleColor::DarkGray);
    } else {
        printColoredAtomic("  Kid Studio Core Tool", ConsoleColor::DarkGray);
    }
    std::cout << _MCDEV_LOG_OUTPUT_ENDL;
}
