#include "mcdevtool/utils.h"
#include <string>

namespace MCDevTool::Utils {
    Version::Version(std::string_view versionStr) {

        uint64_t value = 0;
        bool hasDigit = false;

        for (char c : versionStr) {
            if (c >= '0' && c <= '9') {
                hasDigit = true;
                value = value * 10 + (c - '0');

                // 防止溢出
                if (value > std::numeric_limits<uint32_t>::max()) {
                    components.clear(); // 非法
                    return;
                }
            }
            else if (c == '.') {
                if (!hasDigit) { // 空段：如 "1..2", ".1", "1."
                    components.clear();
                    return;
                }
                components.push_back(static_cast<uint32_t>(value));
                value = 0;
                hasDigit = false;
            }
            else {
                components.clear(); // 非数字非点 -> 非法
                return;
            }
        }

        // 处理最后一段
        if (!hasDigit) { // 例如 "1." → 非法
            components.clear();
            return;
        }
        components.push_back(static_cast<uint32_t>(value));
    }

    bool Version::operator==(const Version& other) const {
        size_t maxLen = std::max(components.size(), other.components.size());
        for (size_t i = 0; i < maxLen; ++i) {
            uint32_t a = (i < components.size()) ? components[i] : 0;
            uint32_t b = (i < other.components.size()) ? other.components[i] : 0;
            if (a != b) {
                return false;
            }
        }
        return true;
    }

    bool Version::operator<(const Version& other) const {
        const auto& a = components;
        const auto& b = other.components;
        size_t n = std::min(a.size(), b.size());

        for (size_t i = 0; i < n; i++) {
            if (a[i] < b[i]) {
                return true;
            }
            if (a[i] > b[i]) {
                return false;
            }
        }
        return a.size() < b.size();
    }
}