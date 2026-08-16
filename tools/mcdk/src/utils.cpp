#include <mcdk/utils.hpp>

namespace mcdk {

    void stringReplace(std::string& value, const std::string& from, const std::string& to) {
        std::size_t startPosition = 0;
        while ((startPosition = value.find(from, startPosition)) != std::string::npos) {
            value.replace(startPosition, from.length(), to);
            startPosition += to.length();
        }
    }

    bool containsIgnoreCase(std::string_view text, std::string_view pattern) {
        if (pattern.empty()) {
            return true;
        }
        if (pattern.size() > text.size()) {
            return false;
        }

        for (std::size_t offset = 0; offset <= text.size() - pattern.size(); ++offset) {
            bool match = true;
            for (std::size_t index = 0; index < pattern.size(); ++index) {
                auto left  = text[offset + index];
                auto right = pattern[index];
                if (left >= 'A' && left <= 'Z') {
                    left += 'a' - 'A';
                }
                if (right >= 'A' && right <= 'Z') {
                    right += 'a' - 'A';
                }
                if (left != right) {
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
