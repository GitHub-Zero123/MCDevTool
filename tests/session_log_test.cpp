#include <mcdk/session_log.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

int main() {
    const auto directory = fs::temp_directory_path() / fs::u8path("mcdk_会话日志_测试");
    const auto path      = directory / "s.log";
    fs::remove_all(directory);
    {
        mcdk::RotatingOutputCapture capture(path, 4096);
        const std::string line(99, 'x');
        for (int i = 0; i < 200; ++i) {
            std::cout << line << '\n';
            std::fprintf(stderr, "e\n");
        }
        std::cout << "LAST" << std::endl;
    }
    auto rotated = path;
    rotated += ".1";
    const bool ok = fs::exists(path) && fs::exists(rotated) && fs::file_size(path) <= 4096 + 16 * 1024
                 && !fs::exists(directory / "s.log.2");
    std::string tail;
    if (ok) {
        std::FILE* file = _wfopen(path.c_str(), L"rb");
        char       buffer[64 * 1024];
        tail.assign(buffer, std::fread(buffer, 1, sizeof(buffer), file));
        std::fclose(file);
    }
    fs::remove_all(directory);
    if (!ok || tail.find("LAST") == std::string::npos) {
        std::cout << "FAILED: rotation or final flush\n";
        return EXIT_FAILURE;
    }
    std::cout << "session log tests passed\n";
    return EXIT_SUCCESS;
}
