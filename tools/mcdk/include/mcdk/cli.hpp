#pragma once

// 面向游戏自动化的命令行入口，命令与退出码见 docs/cli.md。

namespace mcdk::cli {

#ifdef _WIN32
    int parse(int argc, wchar_t* argv[]);
#else
    int parse(int argc, char* argv[]);
#endif

} // namespace mcdk::cli
