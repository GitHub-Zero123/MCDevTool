// Copyright © 2025 GlacieTeam. All rights reserved.
//
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was not
// distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "nbt/detail/SnbtDeserializer.hpp"
#include "nbt/detail/Base64.hpp"
#include "nbt/detail/StringUtils.hpp"
#include "nbt/types/ByteArrayTag.hpp"
#include "nbt/types/ByteTag.hpp"
#include "nbt/types/CompoundTag.hpp"
#include "nbt/types/CompoundTagVariant.hpp"
#include "nbt/types/DoubleTag.hpp"
#include "nbt/types/EndTag.hpp"
#include "nbt/types/FloatTag.hpp"
#include "nbt/types/IntArrayTag.hpp"
#include "nbt/types/IntTag.hpp"
#include "nbt/types/ListTag.hpp"
#include "nbt/types/LongArrayTag.hpp"
#include "nbt/types/LongTag.hpp"
#include "nbt/types/ShortTag.hpp"
#include "nbt/types/StringTag.hpp"
#include <limits>

namespace nbt {

namespace {

static constexpr auto CHAR_EOF = static_cast<char>(std::char_traits<char>::eof());

bool ignoreComment(std::string_view& s) noexcept {
    size_t i = 0;
    switch (s[i++]) {
    case '*': {
        while (i < s.size()) {
            switch (s[i++]) {
            case CHAR_EOF:
            case '\0':
                return false;
            case '*': {
                switch (s[i]) {
                case '/':
                    s.remove_prefix(std::min(i + 1, s.size()));
                    return true;

                default:
                    continue;
                }
            }
            default:
                continue;
            }
        }
        return true;
    }
    default: {
        while (i < s.size()) {
            switch (s[i++]) {
            case '\n':
            case '\r':
            case CHAR_EOF:
            case '\0':
                s.remove_prefix(std::min(i, s.size()));
                return false;

            default:
                break;
            }
        }
        break;
    }
    }
    return false;
}

void scanSpaces(std::string_view& s) noexcept {
    size_t i = 0;
    while (i <= s.size() && std::isspace(s[i++])) {}
    s.remove_prefix(std::min(i - 1, s.size()));
}

bool skipWhitespace(std::string_view& s) {
    scanSpaces(s);
    while (s.starts_with('/') || s.starts_with('#') || s.starts_with(';')) {
        s.remove_prefix(1);
        if (!ignoreComment(s)) { return false; }
        scanSpaces(s);
    }
    return true;
}

char get(std::string_view& s) {
    if (s.empty()) return '\0';
    char c = s.front();
    s.remove_prefix(1);
    return c;
}

std::optional<long double> stold(std::string_view const& s, size_t& n) {
    int&        errnoRef = errno;
    char const* ptr      = s.data();
    char*       eptr;
    errnoRef              = 0;
    const long double res = strtold(ptr, &eptr);
    if (ptr == eptr) { return std::nullopt; }
    if (errnoRef == ERANGE) { return std::nullopt; }
    n = static_cast<size_t>(eptr - ptr);
    return res;
}

template <class R, class T>
std::optional<CompoundTagVariant> checkRange(long double& num) {
    if (std::numeric_limits<T>::lowest() <= num && num <= std::numeric_limits<T>::max()) {
        return R{(T)num};
    } else if constexpr (!std::is_floating_point_v<T>) {
        using unsigned_t = std::make_unsigned_t<T>;
        if (std::numeric_limits<unsigned_t>::lowest() <= num && num <= std::numeric_limits<unsigned_t>::max()) {
            return R{(unsigned_t)num};
        }
    }
    return std::nullopt;
}

std::optional<CompoundTagVariant> parseNumber(std::string_view& s) {
    size_t      n = 0;
    long double res;
    if (auto tmp = stold(s, n); tmp) {
        res = *tmp;
    } else {
        return std::nullopt;
    }
    bool isInt = true;
    for (size_t i = 0; i < n; i++) {
        if (!std::isxdigit(s[i]) && s[i] != '-') { isInt = false; }
    }
    s.remove_prefix(n);
    switch (s.front()) {
    case 'b':
    case 'B':
        s.remove_prefix(1);
        return checkRange<ByteTag, uint8_t>(res);
    case 's':
    case 'S':
        s.remove_prefix(1);
        return checkRange<ShortTag, short>(res);
    case 'i':
    case 'I':
        s.remove_prefix(1);
        return checkRange<IntTag, int>(res);
    case 'l':
    case 'L':
        s.remove_prefix(1);
        return checkRange<LongTag, int64_t>(res);
    case 'f':
    case 'F':
        s.remove_prefix(1);
        return checkRange<FloatTag, float>(res);
    case 'd':
    case 'D':
        s.remove_prefix(1);
        return checkRange<DoubleTag, double>(res);
    default:
        break;
    }
    if (s.size() >= 6) switch (string_utils::doHash(s.substr(0, 6))) {
        case string_utils::doHash(" /*b*/"):
        case string_utils::doHash(" /*B*/"):
            s.remove_prefix(6);
            return checkRange<ByteTag, uint8_t>(res);
        case string_utils::doHash(" /*s*/"):
        case string_utils::doHash(" /*S*/"):
            s.remove_prefix(6);
            return checkRange<ShortTag, short>(res);
        case string_utils::doHash(" /*i*/"):
        case string_utils::doHash(" /*I*/"):
            s.remove_prefix(6);
            return checkRange<IntTag, int>(res);
        case string_utils::doHash(" /*l*/"):
        case string_utils::doHash(" /*L*/"):
            s.remove_prefix(6);
            return checkRange<LongTag, int64_t>(res);
        case string_utils::doHash(" /*f*/"):
        case string_utils::doHash(" /*F*/"):
            s.remove_prefix(6);
            return checkRange<FloatTag, float>(res);
        case string_utils::doHash(" /*d*/"):
        case string_utils::doHash(" /*D*/"):
            s.remove_prefix(6);
            return checkRange<DoubleTag, double>(res);
        default:
            break;
        }
    if (isInt) {
        if (std::numeric_limits<int>::lowest() <= res && res <= std::numeric_limits<int>::max()) {
            return IntTag{(int)res};
        } else {
        }
        if (auto tag = checkRange<IntTag, int>(res)) { return tag; }
        if (auto tag = checkRange<LongTag, int64_t>(res)) { return tag; }
        return std::nullopt;
    } else {
        return DoubleTag{(double)res};
    }
}

std::optional<int> get_codepoint(std::string_view& s) {
    int codepoint = 0;
    for (const auto factor : {12u, 8u, 4u, 0u}) {
        auto current = get(s);
        if (current >= '0' && current <= '9') {
            codepoint += static_cast<int>((static_cast<uint32_t>(current) - 0x30u) << factor);
        } else if (current >= 'A' && current <= 'F') {
            codepoint += static_cast<int>((static_cast<uint32_t>(current) - 0x37u) << factor);
        } else if (current >= 'a' && current <= 'f') {
            codepoint += static_cast<int>((static_cast<uint32_t>(current) - 0x57u) << factor);
        } else {
            return std::nullopt;
        }
    }
    return codepoint;
}

std::optional<std::string> parseString(std::string_view& s) {
    char starts = s.front();
    if (starts != '\"' && starts != '\''
        && !(isalnum(starts) || starts == '-' || starts == '+' || starts == '_' || starts == '.')) {
        return std::nullopt;
    }
    std::string res;
    if (starts == '\"' || starts == '\'') {
        s.remove_prefix(1);
    } else {
        while (!s.empty()) {
            auto fc = s.front();
            if (isalnum(fc) || fc == '-' || fc == '+' || fc == '_' || fc == '.') {
                s.remove_prefix(1);
                res.push_back(fc);
            } else {
                return res;
            }
        }
    }
    while (!s.empty()) {
        auto current = get(s);
        switch (current) {
        case '\"': {
            if (starts == '\"') {
                if (s.starts_with(" /*BASE64*/")) { return base64_utils::decode(res); }
                return res;
            }
            res.push_back('\"');
        } break;
        case '\'': {
            if (starts == '\'') { return res; }
            res.push_back('\'');
        } break;
        case '\\': {
            switch (get(s)) {
            case '\n':
            case '\r':
                if (!skipWhitespace(s)) { return std::nullopt; }
                break;
            case '\"': {
                if (starts == '\"') {
                    res.push_back('\"');
                } else {
                    return std::nullopt;
                }
            } break;
            case '\'': {
                if (starts == '\'') {
                    res.push_back('\'');
                } else {
                    return std::nullopt;
                }
            } break;
            case '\\':
                res.push_back('\\');
                break;
            case '/':
                res.push_back('/');
                break;
            case 'b':
                res.push_back('\b');
                break;
            case 'f':
                res.push_back('\f');
                break;
            case 'n':
                res.push_back('\n');
                break;
            case 'r':
                res.push_back('\r');
                break;
            case 't':
                res.push_back('\t');
                break;
            case 'v':
                res.push_back('\v');
                break;
            case 'u': {
                int codepoint1;
                if (auto getted = get_codepoint(s); getted) {
                    codepoint1 = *getted;
                } else {
                    return std::nullopt;
                }
                int codepoint = codepoint1;
                if (0xD800 <= codepoint1 && codepoint1 <= 0xDBFF) {
                    if (get(s) == '\\' && get(s) == 'u') {
                        int codepoint2;
                        if (auto getted = get_codepoint(s); getted) {
                            codepoint2 = *getted;
                        } else {
                            return std::nullopt;
                        }
                        if ((0xDC00 <= codepoint2 && codepoint2 <= 0xDFFF)) {
                            codepoint = static_cast<int>(
                                (static_cast<uint32_t>(codepoint1) << 10u) + static_cast<uint32_t>(codepoint2)
                                - 0x35FDC00u
                            );
                        } else {
                            return std::nullopt;
                        }
                    } else {
                        return std::nullopt;
                    }
                } else {
                    if (0xDC00 <= codepoint1 && codepoint1 <= 0xDFFF) { return std::nullopt; }
                }
                if (codepoint < 0x80) {
                    res.push_back(static_cast<char>(codepoint));
                } else if (codepoint <= 0x7FF) {
                    res.push_back(static_cast<char>(0xC0u | (static_cast<uint32_t>(codepoint) >> 6u)));
                    res.push_back(static_cast<char>(0x80u | (static_cast<uint32_t>(codepoint) & 0x3Fu)));
                } else if (codepoint <= 0xFFFF) {
                    res.push_back(static_cast<char>(0xE0u | (static_cast<uint32_t>(codepoint) >> 12u)));
                    res.push_back(static_cast<char>(0x80u | ((static_cast<uint32_t>(codepoint) >> 6u) & 0x3Fu)));
                    res.push_back(static_cast<char>(0x80u | (static_cast<uint32_t>(codepoint) & 0x3Fu)));
                } else {
                    res.push_back(static_cast<char>(0xF0u | (static_cast<uint32_t>(codepoint) >> 18u)));
                    res.push_back(static_cast<char>(0x80u | ((static_cast<uint32_t>(codepoint) >> 12u) & 0x3Fu)));
                    res.push_back(static_cast<char>(0x80u | ((static_cast<uint32_t>(codepoint) >> 6u) & 0x3Fu)));
                    res.push_back(static_cast<char>(0x80u | (static_cast<uint32_t>(codepoint) & 0x3Fu)));
                }
            } break;
            default:
                return std::nullopt;
            }
        } break;
        default:
            res.push_back(current);
            break;
        }
    }
    return std::nullopt;
}

template <class R, class T, class H, class F>
std::optional<R> parseNumArray(std::string_view& s, F&& f) {
    T res;
    while (!s.empty()) {
        if (!skipWhitespace(s)) { return std::nullopt; }
        if (s.starts_with(']')) {
            s.remove_prefix(1);
            return res;
        }
        if (auto value = parseNumber(s); value) {
            if (!value->hold<H>()) {
                return std::nullopt;
            } else {
                std::invoke(std::forward<F>(f), res, value->as<H>());
            }
        } else {
            return std::nullopt;
        }
        if (!skipWhitespace(s)) { return std::nullopt; }
        switch (s.front()) {
        case ']':
            s.remove_prefix(1);
            return res;
        case ',':
            s.remove_prefix(1);
        default:
            break;
        }
    }
    return std::nullopt;
}

std::optional<ByteArrayTag> parseByteArray(std::string_view& s) {
    return parseNumArray<ByteArrayTag, std::vector<uint8_t>, ByteTag>(s, [](auto&& vec, auto&& num) {
        vec.emplace_back(num);
    });
}

std::optional<IntArrayTag> parseIntArray(std::string_view& s) {
    return parseNumArray<IntArrayTag, std::vector<int>, IntTag>(s, [](auto&& vec, auto&& num) {
        vec.emplace_back(num);
    });
}

std::optional<LongArrayTag> parseLongArray(std::string_view& s) {
    return parseNumArray<LongArrayTag, std::vector<int64_t>, LongTag>(s, [](auto&& vec, auto&& num) {
        vec.emplace_back(num);
    });
}

std::optional<CompoundTagVariant> parseList(std::string_view& s) {
    if (s.starts_with("[ /*") && (s.size() > 7 && s[6] == '*' && s[7] == '/')) {
        s.remove_prefix(4);
    } else {
        s.remove_prefix(1);
    }
    if (s.starts_with("B;")) {
        s.remove_prefix(2);
        if (s.starts_with("*/")) { s.remove_prefix(2); }
        if (auto array = parseByteArray(s); array) {
            return *array;
        } else {
            return std::nullopt;
        }
    } else if (s.starts_with("I;")) {
        s.remove_prefix(2);
        if (s.starts_with("*/")) { s.remove_prefix(2); }
        if (auto array = parseIntArray(s); array) {
            return *array;
        } else {
            return std::nullopt;
        }
    } else if (s.starts_with("L;")) {
        s.remove_prefix(2);
        if (s.starts_with("*/")) { s.remove_prefix(2); }
        if (auto array = parseLongArray(s); array) {
            return *array;
        } else {
            return std::nullopt;
        }
    }
    ListTag res{};
    while (!s.empty()) {
        if (!skipWhitespace(s)) { return std::nullopt; }
        if (s.starts_with(']')) {
            s.remove_prefix(1);
            if (!res.empty()) res.getElementType() = res.storage().front()->getType();
            return res;
        }
        auto value = detail::parseSnbtValueNonSkip(s);
        if (!value) { return std::nullopt; }
        res.push_back(std::move(*value).toUnique());

        if (!skipWhitespace(s)) { return std::nullopt; }
        switch (s.front()) {
        case ']':
            s.remove_prefix(1);
            res.getElementType() = res.storage().front()->getType();
            return res;
        case ',': {
            s.remove_prefix(1);
        }
        default:
            break;
        }
    }
    return std::nullopt;
}

std::optional<CompoundTagVariant> parseCompound(std::string_view& s) {
    get(s);
    if (!skipWhitespace(s)) { return std::nullopt; }
    if (s.starts_with('}')) {
        s.remove_prefix(1);
        return CompoundTag{};
    }
    CompoundTag res;
    while (!s.empty()) {
        if (!skipWhitespace(s)) { return std::nullopt; }
        if (s.starts_with('}')) {
            s.remove_prefix(1);
            return res;
        }
        auto key = parseString(s);
        if (!key) { return std::nullopt; }
        if (!skipWhitespace(s)) { return std::nullopt; }
        auto p = get(s);
        if (p != ':' && p != '=') { return std::nullopt; }
        auto value = detail::parseSnbtValue(s);
        if (!value) { return std::nullopt; }
        res[*key] = *value;

        switch (s.front()) {
        case '}':
            s.remove_prefix(1);
            return res;
        case ',':
            s.remove_prefix(1);
        default:
            break;
        }
    }
    return std::nullopt;
}

} // namespace

namespace detail {

std::optional<CompoundTagVariant> parseSnbtValueNonSkip(std::string_view& s) {
    if (s.empty()) { return std::nullopt; }
    switch (s.front()) {
    case 't':
        if (s.starts_with("true")) {
            s.remove_prefix(4);
            return ByteTag{true};
        }
        break;
    case 'f':
        if (s.starts_with("false")) {
            s.remove_prefix(5);
            return ByteTag{false};
        }
        break;
    case 'n':
        if (s.starts_with("null")) {
            s.remove_prefix(4);
            return EndTag{};
        }
        break;
    case ']':
    case '}':
        s.remove_prefix(1);
        return std::nullopt;
    case CHAR_EOF:
    case '\0':
        return std::nullopt;
    case '-':
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
        return parseNumber(s);
    case '[':
        return parseList(s);
    case '{':
        return parseCompound(s);
    default:
        break;
    }
    return parseString(s);
}

std::optional<CompoundTagVariant> parseSnbtValue(std::string_view& s) {
    if (!skipWhitespace(s)) { return std::nullopt; }
    auto res = parseSnbtValueNonSkip(s);
    if (!res) { return res; }
    if (!skipWhitespace(s)) { return std::nullopt; }
    return res;
}

} // namespace detail

} // namespace nbt
