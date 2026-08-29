#include <mcdk/port_range.hpp>

#include <algorithm>
#include <charconv>
#include <cctype>

namespace mcdk {

    namespace {

        std::string_view trim(std::string_view value) {
            const auto isSpace = [](char ch) { return std::isspace(static_cast<unsigned char>(ch)) != 0; };
            while (!value.empty() && isSpace(value.front())) {
                value.remove_prefix(1);
            }
            while (!value.empty() && isSpace(value.back())) {
                value.remove_suffix(1);
            }
            return value;
        }

        bool parseNumber(std::string_view text, int& out) {
            text = trim(text);
            // 端口号只能是十进制正整数；显式拒绝符号位，否则 "-19133" 会被当成负数解析。
            if (text.empty() || text.find_first_not_of("0123456789") != std::string_view::npos) {
                return false;
            }
            int        value  = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
                return false;
            }
            out = value;
            return true;
        }

        // 返回分隔符位置 区间写法允许 '-' '..' ':' 三种形式
        std::size_t findSeparator(std::string_view text, std::size_t& separatorLength) {
            if (const auto dots = text.find(".."); dots != std::string_view::npos) {
                separatorLength = 2;
                return dots;
            }
            // 端口号不会为负 首字符的 '-' 一律视为非法输入而不是分隔符
            if (const auto dash = text.find('-', 1); dash != std::string_view::npos) {
                separatorLength = 1;
                return dash;
            }
            if (const auto colon = text.find(':', 1); colon != std::string_view::npos) {
                separatorLength = 1;
                return colon;
            }
            separatorLength = 0;
            return std::string_view::npos;
        }

        int clampPort(int port) { return std::clamp(port, PortRange::MinPort, PortRange::MaxPort); }

    } // namespace

    PortRange PortRange::normalized(int first, int second) {
        // 用户可能刻意先写大后写小 这里统一按升序处理而不是报错
        if (first > second) {
            std::swap(first, second);
        }
        PortRange range{clampPort(first), clampPort(second)};
        if (range.size() > MaxSpan) {
            range.end = range.begin + MaxSpan - 1;
        }
        return range;
    }

    std::vector<int> PortRange::ports() const {
        std::vector<int> result;
        result.reserve(static_cast<std::size_t>(size()));
        for (int port = begin; port <= end; ++port) {
            result.push_back(port);
        }
        return result;
    }

    std::string PortRange::toString() const {
        if (isSingle()) {
            return std::to_string(begin);
        }
        return std::to_string(begin) + "-" + std::to_string(end);
    }

    namespace {

        // 归一化过程中如果调整了用户写下的值 就把原因说清楚 便于排查配置
        PortRangeParseResult finalize(int first, int second) {
            PortRangeParseResult result;
            result.range = PortRange::normalized(first, second);
            result.ok    = true;

            const int requestedLow  = std::min(first, second);
            const int requestedHigh = std::max(first, second);
            if (first > second) {
                result.warning = "端口区间起止顺序颠倒 已按升序处理为 " + result.range.toString();
            }
            if (requestedLow < PortRange::MinPort || requestedHigh > PortRange::MaxPort) {
                result.warning = "端口区间超出 1-65535 已钳制为 " + result.range.toString();
            } else if (requestedHigh - requestedLow + 1 > PortRange::MaxSpan) {
                result.warning = "端口区间跨度超过 " + std::to_string(PortRange::MaxSpan) + " 已截断为 "
                               + result.range.toString();
            }
            return result;
        }

    } // namespace

    PortRangeParseResult makePortRange(int first, int second) { return finalize(first, second); }

    PortRangeParseResult parsePortRange(std::string_view text, PortRange fallback) {
        PortRangeParseResult result;
        result.range = fallback;

        const auto trimmed = trim(text);
        if (trimmed.empty()) {
            result.error = "端口配置为空";
            return result;
        }

        std::size_t separatorLength = 0;
        const auto  separator       = findSeparator(trimmed, separatorLength);
        if (separator == std::string_view::npos) {
            // 单值端口 兼容旧版本配置
            int port = 0;
            if (!parseNumber(trimmed, port)) {
                result.error = "无法解析端口 \"" + std::string(trimmed) + "\"";
                return result;
            }
            return finalize(port, port);
        }

        int first  = 0;
        int second = 0;
        if (!parseNumber(trimmed.substr(0, separator), first)
            || !parseNumber(trimmed.substr(separator + separatorLength), second)) {
            result.error = "无法解析端口区间 \"" + std::string(trimmed) + "\"";
            return result;
        }
        return finalize(first, second);
    }

} // namespace mcdk
