#pragma once
#include <string>
#include <string_view>

namespace mcdk {

    // 字符串关键字替换
    inline void stringReplace(std::string& str, const std::string& from, const std::string& to) {
        size_t startPos = 0;
        while ((startPos = str.find(from, startPos)) != std::string::npos) {
            str.replace(startPos, from.length(), to);
            startPos += to.length();
        }
    }

    // 不区分大小写的字符串包含检查（ASCII only）
    inline bool containsIgnoreCase(std::string_view text, std::string_view pattern) {
        if (pattern.empty()) {
            return true;
        }

        if (pattern.size() > text.size()) {
            return false;
        }

        for (size_t i = 0; i <= text.size() - pattern.size(); ++i) {
            bool match = true;
            for (size_t j = 0; j < pattern.size(); ++j) {
                char a = text[i + j];
                char b = pattern[j];

                // ASCII tolower（无 locale）
                if (a >= 'A' && a <= 'Z') {
                    a += 'a' - 'A';
                }
                if (b >= 'A' && b <= 'Z') {
                    b += 'a' - 'A';
                }

                if (a != b) {
                    match = false;
                    break;
                }
            }
            if (match) {
                return true;
            }
        }
        return false;
    }

} // namespace mcdk
