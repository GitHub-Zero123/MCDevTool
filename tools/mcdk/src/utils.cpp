#include <mcdk/utils.hpp>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace mcdk {

    std::string utcTimestampNow() {
        const auto now = std::chrono::system_clock::now();
        const auto milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        const auto time = std::chrono::system_clock::to_time_t(now);
        std::tm    utc{};
#ifdef _WIN32
        gmtime_s(&utc, &time);
#else
        gmtime_r(&time, &utc);
#endif
        std::ostringstream output;
        output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0')
               << milliseconds.count() << 'Z';
        return output.str();
    }

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
