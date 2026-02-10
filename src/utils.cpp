#include "mcdevtool/utils.h"
#include <algorithm>
#include <string>
#include <ctime>

namespace MCDevTool::Utils {
    Version::Version(std::string_view versionStr) {

        uint64_t value    = 0;
        bool     hasDigit = false;

        for (char c : versionStr) {
            if (c >= '0' && c <= '9') {
                hasDigit = true;
                value    = value * 10 + (c - '0');

                // 防止溢出
                if (value > std::numeric_limits<uint32_t>::max()) {
                    components.clear(); // 非法
                    return;
                }
            } else if (c == '.') {
                if (!hasDigit) { // 空段：如 "1..2", ".1", "1."
                    components.clear();
                    return;
                }
                components.push_back(static_cast<uint32_t>(value));
                value    = 0;
                hasDigit = false;
            } else {
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
        size_t      n = std::min(a.size(), b.size());

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

    static char _RANDOM_HEX() {
        int value = rand() % 16;
        return (value < 10) ? ('0' + value) : ('A' + (value - 10));
    }

    static void _GENERATE_UUID(char* uuid) {
        static bool _init = false;
        if (!_init) {
            _init = true; // 初始化种子
            srand((unsigned int)std::time(NULL));
        }
        const int   UUID_LENGTH = 36;                                     // UUID 长度（包含分隔符）
        const char* format      = "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx"; // UUID 模板
        for (int i = 0; i < UUID_LENGTH; ++i) {
            switch (format[i]) {
            case 'x':
                uuid[i] = _RANDOM_HEX();
                break;
            case '4':
                uuid[i] = '4'; // 固定版本号为 4
                break;
            case 'y':
                uuid[i] = "89AB"[rand() % 4]; // 固定 variant 为 8, 9, A 或 B
                break;
            case '-':
                uuid[i] = '-'; // 分隔符
                break;
            default:
                break;
            }
        }
        uuid[UUID_LENGTH] = '\0'; // 添加字符串结束符
    }

    // 生成随机UUID字符串
    std::string createRandomUUID() {
        char uuid[37];
        _GENERATE_UUID(uuid);
        return std::string(uuid, sizeof(uuid) - 1);
    }

    // 生成去除-符号的UUID字符串
    std::string createCompactUUID() {
        std::string rawUUID = createRandomUUID();
        rawUUID.erase(std::remove(rawUUID.begin(), rawUUID.end(), '-'), rawUUID.end());
        return rawUUID;
    }


    // 将 std::filesystem::path 安全转换为 UTF-8 std::string（避免 ANSI 代码页映射失败）
    std::string pathToUtf8(const std::filesystem::path& p) {
        auto u8s = p.u8string();
        return std::string(u8s.begin(), u8s.end());
    }

    // 将 std::filesystem::path 安全转换为 UTF-8 generic（正斜杠）std::string
    std::string pathToGenericUtf8(const std::filesystem::path& p) {
        auto u8s = p.generic_u8string();
        return std::string(u8s.begin(), u8s.end());
    }
} // namespace MCDevTool::Utils