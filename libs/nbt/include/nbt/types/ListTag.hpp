// Copyright © 2025 GlacieTeam. All rights reserved.
//
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was not
// distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once
#include <functional>
#include <nbt/types/Tag.hpp>
#include <vector>

namespace nbt {

class CompoundTag;
class CompoundTagVariant;

class ListTag : public Tag {
public:
    using TagList = std::vector<std::unique_ptr<Tag>>;

public:
    TagList mStorage{};
    Type    mType{Type::End};

public:
    using iterator               = TagList::iterator;
    using const_iterator         = TagList::const_iterator;
    using reverse_iterator       = TagList::reverse_iterator;
    using const_reverse_iterator = TagList::const_reverse_iterator;

public:
    [[nodiscard]] NBT_API ListTag() = default;
    [[nodiscard]] NBT_API ListTag(TagList&& data);
    [[nodiscard]] NBT_API ListTag(ListTag const& other);
    [[nodiscard]] NBT_API ListTag(ListTag&& other);
    [[nodiscard]] NBT_API ListTag(std::initializer_list<CompoundTagVariant> tags);
    [[nodiscard]] NBT_API ListTag(std::vector<CompoundTagVariant> tags);

    NBT_API ListTag& operator=(ListTag const& other);
    NBT_API ListTag& operator=(ListTag&& other);

    [[nodiscard]] NBT_API Type getType() const override;

    [[nodiscard]] NBT_API bool equals(Tag const& other) const override;

    [[nodiscard]] NBT_API std::unique_ptr<Tag> copy() const override;

    [[nodiscard]] NBT_API std::size_t hash() const override;

    NBT_API void write(io::BytesDataOutput& stream) const override;

    NBT_API void load(io::BytesDataInput& stream) override;

    NBT_API void write(bstream::BinaryStream& stream) const override;

    NBT_API void load(bstream::ReadOnlyBinaryStream& stream) override;

    NBT_API void merge(ListTag const& other);

public:
    [[nodiscard]] NBT_API std::unique_ptr<ListTag> copyList() const;

    [[nodiscard]] NBT_API size_t size() const noexcept;
    [[nodiscard]] NBT_API bool   empty() const;

    [[nodiscard]] NBT_API Type const& getElementType() const;
    [[nodiscard]] NBT_API Type&       getElementType();

    NBT_API void push_back(std::unique_ptr<Tag>&& tag);
    NBT_API void push_back(Tag const& tag);
    NBT_API void push_back(CompoundTagVariant val);

    NBT_API void reserve(size_t size);

    NBT_API bool remove(size_t index);
    NBT_API bool remove(size_t startIndex, size_t endIndex);
    NBT_API void clear() noexcept;

    [[nodiscard]] NBT_API TagList&       storage() noexcept;
    [[nodiscard]] NBT_API TagList const& storage() const noexcept;

    NBT_API void forEachCompoundTag(std::function<void(CompoundTag const& tag)> func);

    [[nodiscard]] NBT_API Tag&       operator[](size_t index) noexcept;
    [[nodiscard]] NBT_API Tag const& operator[](size_t index) const noexcept;

    [[nodiscard]] NBT_API Tag&       at(size_t index);
    [[nodiscard]] NBT_API Tag const& at(size_t index) const;

    [[nodiscard]] NBT_API iterator begin() noexcept;
    [[nodiscard]] NBT_API iterator end() noexcept;

    [[nodiscard]] NBT_API const_iterator begin() const noexcept;
    [[nodiscard]] NBT_API const_iterator end() const noexcept;

    [[nodiscard]] NBT_API const_iterator cbegin() const noexcept;
    [[nodiscard]] NBT_API const_iterator cend() const noexcept;

    [[nodiscard]] NBT_API reverse_iterator rbegin() noexcept;
    [[nodiscard]] NBT_API reverse_iterator rend() noexcept;

    [[nodiscard]] NBT_API const_reverse_iterator crbegin() const noexcept;
    [[nodiscard]] NBT_API const_reverse_iterator crend() const noexcept;

    NBT_API iterator erase(const_iterator where) noexcept;
    NBT_API iterator erase(const_iterator first, const_iterator last) noexcept;

    NBT_API bool set(size_t index, Tag const& tag);
};

} // namespace nbt