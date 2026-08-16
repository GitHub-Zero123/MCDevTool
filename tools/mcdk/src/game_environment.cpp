#include <mcdk/game_environment.hpp>

#include <algorithm>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace mcdk {
    namespace {
        std::wstring utf8ToUtf16(std::string_view value) {
            if (value.empty()) {
                return {};
            }

            const auto size = MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                value.data(),
                static_cast<int>(value.size()),
                nullptr,
                0
            );
            if (size == 0) {
                throw std::runtime_error("Failed to convert a game environment value from UTF-8 to UTF-16.");
            }

            std::wstring result(static_cast<std::size_t>(size), L'\0');
            if (MultiByteToWideChar(
                    CP_UTF8,
                    MB_ERR_INVALID_CHARS,
                    value.data(),
                    static_cast<int>(value.size()),
                    result.data(),
                    size
                ) == 0) {
                throw std::runtime_error("Failed to convert a game environment value from UTF-8 to UTF-16.");
            }
            return result;
        }

        std::wstring_view environmentVariableName(std::wstring_view entry) {
            const auto searchStart = entry.starts_with(L'=') ? 1U : 0U;
            const auto separator   = entry.find(L'=', searchStart);
            if (separator == std::wstring_view::npos) {
                return {};
            }
            return entry.substr(0, separator);
        }

        bool equalsIgnoreCase(std::wstring_view left, std::wstring_view right) {
            return CompareStringOrdinal(
                       left.data(),
                       static_cast<int>(left.size()),
                       right.data(),
                       static_cast<int>(right.size()),
                       TRUE
                   ) == CSTR_EQUAL;
        }

        bool isMcdkVariable(std::wstring_view name) {
            constexpr std::wstring_view prefix = L"MCDEV_";
            return name.size() >= prefix.size() && equalsIgnoreCase(name.substr(0, prefix.size()), prefix);
        }

    } // namespace

    GameEnvironmentBuilder& GameEnvironmentBuilder::set(std::wstring_view name, std::wstring value) {
        if (name.empty() || name.find(L'=') != std::wstring_view::npos
            || name.find(L'\0') != std::wstring_view::npos || value.find(L'\0') != std::wstring::npos) {
            throw std::invalid_argument("Invalid game environment variable name or value.");
        }

        std::erase_if(mEntries, [name](const std::wstring& entry) {
            return equalsIgnoreCase(environmentVariableName(entry), name);
        });
        std::wstring entry(name);
        entry.reserve(name.size() + value.size() + 1);
        entry.push_back(L'=');
        entry.append(std::move(value));
        mEntries.push_back(std::move(entry));
        return *this;
    }

    GameEnvironmentBuilder& GameEnvironmentBuilder::setUtf8(std::wstring_view name, std::string_view value) {
        return set(name, utf8ToUtf16(value));
    }

    std::wstring GameEnvironmentBuilder::build() && {
        auto environment = std::unique_ptr<wchar_t, decltype(&FreeEnvironmentStringsW)>(
            GetEnvironmentStringsW(),
            FreeEnvironmentStringsW
        );
        if (!environment) {
            throw std::runtime_error("Failed to read the current process environment.");
        }

        std::vector<std::wstring> entries;
        entries.reserve(mEntries.size() + 64);
        for (auto* current = environment.get(); *current != L'\0';) {
            std::wstring entry(current);
            current += entry.size() + 1;

            const auto name = environmentVariableName(entry);
            const bool overridden = std::ranges::any_of(mEntries, [name](const std::wstring& overrideEntry) {
                return equalsIgnoreCase(environmentVariableName(overrideEntry), name);
            });
            // MCDEV_ is private to MCDK unless a business layer explicitly adds it to this builder.
            if (!isMcdkVariable(name) && !overridden) {
                entries.push_back(std::move(entry));
            }
        }
        entries.insert(
            entries.end(),
            std::make_move_iterator(mEntries.begin()),
            std::make_move_iterator(mEntries.end())
        );

        std::ranges::sort(entries, [](const std::wstring& left, const std::wstring& right) {
            const auto comparison = CompareStringOrdinal(
                left.data(),
                static_cast<int>(left.size()),
                right.data(),
                static_cast<int>(right.size()),
                TRUE
            );
            if (comparison == CSTR_EQUAL) {
                return left < right;
            }
            return comparison == CSTR_LESS_THAN;
        });

        std::size_t size = 1;
        for (const auto& entry : entries) {
            size += entry.size() + 1;
        }
        if (entries.empty()) {
            ++size;
        }

        std::wstring block;
        block.reserve(size);
        for (const auto& entry : entries) {
            block.append(entry);
            block.push_back(L'\0');
        }
        block.push_back(L'\0');
        if (entries.empty()) {
            block.push_back(L'\0');
        }
        return block;
    }

} // namespace mcdk
