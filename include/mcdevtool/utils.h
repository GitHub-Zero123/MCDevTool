#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <string_view>

namespace MCDevTool::Utils {
    class Version {
    public:
        Version() = default;
        Version(std::string_view versionStr);
        bool operator<(const Version& other) const;
        bool operator==(const Version& other) const;

        explicit operator bool() const {
            return !components.empty();
        }
    private:
        std::vector<uint32_t> components;
    };

    // 生成随机UUID字符串
    std::string createRandomUUID();

    // 生成去除-符号的UUID字符串
    std::string createCompactUUID();
} // namespace MCDevTool::Utils