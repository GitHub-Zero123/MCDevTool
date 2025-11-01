// Copyright © 2025 GlacieTeam. All rights reserved.
//
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was not
// distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

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
#include "nbt/types/LongTag.hpp"
#include "nbt/types/ShortTag.hpp"
#include "nbt/types/StringTag.hpp"
#include <cmath>
#include <format>
#include <limits>

namespace nbt {

namespace {

template <std::integral T>
std::string toString(T value) {
    return std::format("{}", value);
}

template <std::floating_point T>
std::string toString(T value) {
    if (std::round(value) == value) { return std::format("{:.1f}", value); }
    return std::format("{}", value);
}

template <std::integral T>
constexpr std::string makeSnbtTagValue(T value, SnbtFormat format, char mark) {
    bool upper = static_cast<bool>(format & SnbtFormat::ForceUppercase);
    if (static_cast<bool>(format & SnbtFormat::CommentMarks)) {
        return std::format("{0} /*{1:c}*/", value, upper ? std::toupper(mark) : mark);
    }
    return std::format("{0}{1:c}", value, upper ? std::toupper(mark) : mark);
}

template <std::floating_point T>
constexpr std::string makeSnbtTagValue(T value, SnbtFormat format, char mark) {
    bool upper = static_cast<bool>(format & SnbtFormat::ForceUppercase);
    if (static_cast<bool>(format & SnbtFormat::CommentMarks)) {
        if (std::round(value) == value) {
            return std::format("{0:.1f} /*{1:c}*/", value, upper ? std::toupper(mark) : mark);
        }
        return std::format("{0} /*{1:c}*/", value, upper ? std::toupper(mark) : mark);
    }
    if (std::round(value) == value) { return std::format("{0:.1f}{1:c}", value, upper ? std::toupper(mark) : mark); }
    return std::format("{0}{1:c}", value, upper ? std::toupper(mark) : mark);
}

bool isMinimize(SnbtFormat format) {
    return !(
        static_cast<bool>(format & SnbtFormat::CompoundLineFeed)
        || static_cast<bool>(format & SnbtFormat::ListArrayLineFeed)
        || static_cast<bool>(format & SnbtFormat::BinaryArrayLineFeed)
    );
}

std::string toDumpString(std::string const& str, SnbtFormat format, bool key) {
    std::string res;
    if (str.empty()) {
        res = "\"\"";
    } else {
        bool isTrivial = true;
        if (!static_cast<bool>(format & SnbtFormat::ForceQuote)) {
            if (!key && (str[0] == '-' || isdigit(str[0]))) {
                isTrivial = false;
            } else {
                for (auto c : str) {
                    if (!(isalnum(c) || c == '-' || c == '+' || c == '_' || c == '.')) {
                        isTrivial = false;
                        break;
                    }
                }
            }
        } else {
            isTrivial = false;
        }
        if (isTrivial) {
            res = str;
        } else {
            if (string_utils::isValidUTF8(str)) {
                res = res = std::format(
                    "\"{}\"",
                    string_utils::dumpString(str, static_cast<bool>(format & SnbtFormat::ForceAscii))
                );
            } else {
                res = std::format("\"{}\" /*BASE64*/", base64_utils::encode(str));
            }
        }
    }
    return res;
}

} // namespace

namespace detail {

std::string TypedToSnbt(EndTag const&, uint8_t, SnbtFormat, bool) { return "null"; }

std::string TypedToSnbt(ByteTag const& self, uint8_t, SnbtFormat format, bool dumpJson) {
    if (dumpJson) return toString(self.storage());
    return makeSnbtTagValue(self.storage(), format, 'b');
}

std::string TypedToSnbt(ShortTag const& self, uint8_t, SnbtFormat format, bool dumpJson) {
    if (dumpJson) return toString(self.storage());
    return makeSnbtTagValue(self.storage(), format, 's');
}

std::string TypedToSnbt(IntTag const& self, uint8_t, SnbtFormat format, bool dumpJson) {
    if (dumpJson) return toString(self.storage());
    if (static_cast<bool>(format & SnbtFormat::MarkIntTag)) { return makeSnbtTagValue(self.storage(), format, 'i'); }
    return toString(self.storage());
}

std::string TypedToSnbt(LongTag const& self, uint8_t, SnbtFormat format, bool dumpJson) {
    if (dumpJson) return toString(self.storage());
    return makeSnbtTagValue(self.storage(), format, 'l');
}

std::string TypedToSnbt(FloatTag const& self, uint8_t, SnbtFormat format, bool dumpJson) {
    if (dumpJson) return toString(self.storage());
    return makeSnbtTagValue(self.storage(), format, 'f');
}

std::string TypedToSnbt(DoubleTag const& self, uint8_t, SnbtFormat format, bool dumpJson) {
    if (dumpJson) return toString(self.storage());
    if (static_cast<bool>(format & SnbtFormat::MarkDoubleTag)) { return makeSnbtTagValue(self.storage(), format, 'd'); }
    return toString(self.storage());
}

std::string TypedToSnbt(StringTag const& self, uint8_t, SnbtFormat format, bool) {
    return toDumpString(self.storage(), format, false);
}

std::string TypedToSnbt(ListTag const& self, uint8_t indent, SnbtFormat format, bool dumpJson) {
    std::string res;

    static constexpr std::string_view lbracket{"["}, rbracket{"]"};

    res += lbracket;

    size_t      i = self.size();
    std::string indentSpace(indent, ' ');

    bool isMinimized = isMinimize(format);
    bool isNewLine   = static_cast<bool>(format & SnbtFormat::ListArrayLineFeed)
                  && (static_cast<bool>(format & SnbtFormat::ForceLineFeedIgnoreIndent) || (indent > 0));

    if (isNewLine && self.size() > 0) { res += '\n'; }
    for (auto& tag : self) {
        i--;
        if (isNewLine) { res += indentSpace; }
        auto key = tag->toSnbt(format, indent);
        if (dumpJson) { key = tag->toJson(); }

        if (isNewLine) { string_utils::replaceAll(key, "\n", "\n" + indentSpace); }
        res += key;

        if (i > 0) {
            res += ',';
            if (!isMinimized && !isNewLine) { res += ' '; }
        }
        if (isNewLine) { res += '\n'; }
    }

    res += rbracket;

    return res;
}

std::string TypedToSnbt(CompoundTag const& self, uint8_t indent, SnbtFormat format, bool dumpJson) {

    std::string res;

    static constexpr std::string_view lbracket{"{"}, rbracket{"}"};

    res += lbracket;

    size_t      i = self.size();
    std::string indentSpace(indent, ' ');

    bool isMinimized = isMinimize(format);
    bool isNewLine   = static_cast<bool>(format & SnbtFormat::CompoundLineFeed)
                  && (static_cast<bool>(format & SnbtFormat::ForceLineFeedIgnoreIndent) || (indent > 0));

    if (isNewLine && self.size() > 0) { res += '\n'; }

    for (auto& [k, v] : self.items()) {
        i--;
        if (isNewLine) { res += indentSpace; }
        auto keyStr = toDumpString(k, format, true);

        res += keyStr;

        res += ':';

        if (!isMinimized) { res += ' '; }
        auto key = v.toSnbt(format, indent);
        if (dumpJson) { key = v.toJson(); }

        if (isNewLine) { string_utils::replaceAll(key, "\n", "\n" + indentSpace); }
        res += key;

        if (i > 0) {
            res += ',';
            if (!isMinimized && !isNewLine) { res += ' '; }
        }
        if (isNewLine) { res += '\n'; }
    }

    res += rbracket;

    return res;
}

std::string TypedToSnbt(ByteArrayTag const& self, uint8_t indent, SnbtFormat format, bool dumpJson) {

    std::string res;

    std::string lbracket{"[B;"}, rbracket{"]"};
    if (static_cast<bool>(format & SnbtFormat::CommentMarks)) { lbracket = "[ /*B;*/"; }
    if (dumpJson) { lbracket = "["; }

    res += lbracket;

    size_t      i = self.size();
    std::string indentSpace(indent, ' ');

    bool isMinimized = isMinimize(format);
    bool isNewLine   = static_cast<bool>(format & SnbtFormat::BinaryArrayLineFeed)
                  && (static_cast<bool>(format & SnbtFormat::ForceLineFeedIgnoreIndent) || (indent > 0));

    if (isNewLine && self.size() > 0) { res += '\n'; }

    for (auto& tag : self.storage()) {
        i--;
        if (isNewLine) { res += indentSpace; }

        if (dumpJson) {
            res += toString(tag);
        } else {
            res += makeSnbtTagValue(tag, format, 'b');
        }

        if (i > 0) {
            res += ',';
            if (!isMinimized && !isNewLine) { res += ' '; }
        }
        if (isNewLine) { res += '\n'; }
    }

    res += rbracket;

    return res;
}

std::string TypedToSnbt(IntArrayTag const& self, uint8_t indent, SnbtFormat format, bool dumpJson) {

    std::string res;

    std::string lbracket{"[I;"}, rbracket{"]"};
    if (static_cast<bool>(format & SnbtFormat::CommentMarks)) { lbracket = "[ /*I;*/"; }
    if (dumpJson) { lbracket = "["; }

    res += lbracket;

    size_t      i = self.size();
    std::string indentSpace(indent, ' ');

    bool isMinimized = isMinimize(format);
    bool isNewLine   = static_cast<bool>(format & SnbtFormat::BinaryArrayLineFeed)
                  && (static_cast<bool>(format & SnbtFormat::ForceLineFeedIgnoreIndent) || (indent > 0));

    if (isNewLine && self.size() > 0) { res += '\n'; }

    for (auto& tag : self.storage()) {
        i--;
        if (isNewLine) { res += indentSpace; }

        if (dumpJson) {
            res += toString(tag);
        } else {
            res += makeSnbtTagValue(tag, format, 'i');
        }

        if (i > 0) {
            res += ',';
            if (!isMinimized && !isNewLine) { res += ' '; }
        }
        if (isNewLine) { res += '\n'; }
    }

    res += rbracket;

    return res;
}

std::string TypedToSnbt(LongArrayTag const& self, uint8_t indent, SnbtFormat format, bool dumpJson) {

    std::string res;

    std::string lbracket{"[L;"}, rbracket{"]"};
    if (static_cast<bool>(format & SnbtFormat::CommentMarks)) { lbracket = "[ /*L;*/"; }
    if (dumpJson) { lbracket = "["; }

    res += lbracket;

    size_t      i = self.size();
    std::string indentSpace(indent, ' ');

    bool isMinimized = isMinimize(format);
    bool isNewLine   = static_cast<bool>(format & SnbtFormat::BinaryArrayLineFeed)
                  && (static_cast<bool>(format & SnbtFormat::ForceLineFeedIgnoreIndent) || (indent > 0));

    if (isNewLine && self.size() > 0) { res += '\n'; }

    for (auto& tag : self.storage()) {
        i--;
        if (isNewLine) { res += indentSpace; }

        if (dumpJson) {
            res += toString(tag);
        } else {
            res += makeSnbtTagValue(tag, format, 'l');
        }

        if (i > 0) {
            res += ',';
            if (!isMinimized && !isNewLine) { res += ' '; }
        }
        if (isNewLine) { res += '\n'; }
    }

    res += rbracket;

    return res;
}

} // namespace detail

} // namespace nbt