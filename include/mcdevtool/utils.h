#pragma once
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
} // namespace MCDevTool::Utils