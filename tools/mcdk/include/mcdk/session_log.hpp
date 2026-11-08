#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

namespace mcdk {

    // 把本进程的 stdout/stderr 接到按大小轮转的日志文件：写满后改名为 <file>.1 再开新文件，
    // 磁盘上最多两份。后台会话长时间运行时日志不会无限增长。
    class RotatingOutputCapture {
    public:
        static constexpr std::uintmax_t DefaultMaxBytes = 8u * 1024u * 1024u;

        RotatingOutputCapture(std::filesystem::path path, std::uintmax_t maxBytes = DefaultMaxBytes);
        // 冲刷并写完管道里剩余的内容。
        ~RotatingOutputCapture();

        RotatingOutputCapture(const RotatingOutputCapture&)            = delete;
        RotatingOutputCapture& operator=(const RotatingOutputCapture&) = delete;

    private:
        struct State;
        std::unique_ptr<State> mState;
    };

} // namespace mcdk
