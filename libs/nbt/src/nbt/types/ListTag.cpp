// Copyright © 2025 GlacieTeam. All rights reserved.
//
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was not
// distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "nbt/types/ListTag.hpp"
#include "nbt/types/CompoundTagVariant.hpp"
#include <algorithm>

namespace nbt {

ListTag::ListTag(TagList&& data) : mStorage(std::move(data)) {
    if (!mStorage.empty()) { mType = mStorage.front()->getType(); }
}

ListTag::ListTag(ListTag const& other) : mType(other.mType) {
    for (const auto& data : other.mStorage) { mStorage.emplace_back(data.get()->copy()); }
}

ListTag::ListTag(ListTag&& other) = default;

ListTag::ListTag(std::initializer_list<CompoundTagVariant> tags) : ListTag(std::vector<CompoundTagVariant>(tags)) {}

ListTag::ListTag(std::vector<CompoundTagVariant> tags) {
    for (auto& tag : tags) { mStorage.emplace_back(tag->copy()); }
    if (!mStorage.empty()) { mType = mStorage.front()->getType(); }
}

ListTag& ListTag::operator=(ListTag const& other) {
    mStorage.clear();
    mType = other.mType;
    for (const auto& data : other.mStorage) { mStorage.emplace_back(data->copy()); }
    return *this;
}

ListTag& ListTag::operator=(ListTag&& other) = default;

bool ListTag::equals(const Tag& other) const {
    return (other.getType() == Type::List) && (mStorage == static_cast<const ListTag&>(other).mStorage);
}

Tag::Type ListTag::getType() const { return Type::List; }

std::unique_ptr<Tag> ListTag::copy() const { return copyList(); }

std::size_t ListTag::hash() const {
    std::size_t hash = 0;
    for (auto& value : mStorage) {
        std::size_t element_hash  = value.get()->hash();
        hash                     ^= element_hash + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    }
    return hash;
}

std::unique_ptr<ListTag> ListTag::copyList() const {
    auto copy   = std::make_unique<ListTag>();
    copy->mType = mType;
    for (const auto& data : mStorage) { copy->mStorage.emplace_back(data->copy()); }
    return copy;
}

void ListTag::write(io::BytesDataOutput& stream) const {
    stream.writeByte((uint8_t)mType);
    stream.writeInt((int)mStorage.size());
    for (const auto& data : mStorage) { data->write(stream); }
}

void ListTag::load(io::BytesDataInput& stream) {
    mType     = static_cast<Type>(stream.getByte());
    auto size = stream.getInt();
    for (int i = 0; i < size; i++) {
        if (auto tag = Tag::newTag(mType)) {
            tag->load(stream);
            mStorage.push_back(std::move(tag));
        }
    }
}

void ListTag::write(bstream::BinaryStream& stream) const {
    stream.writeUnsignedChar((uint8_t)mType);
    stream.writeVarInt((int)mStorage.size());
    for (const auto& data : mStorage) { data->write(stream); }
}

void ListTag::load(bstream::ReadOnlyBinaryStream& stream) {
    mType     = static_cast<Type>(stream.getUnsignedChar());
    auto size = stream.getVarInt();
    for (int i = 0; i < size; i++) {
        if (auto tag = Tag::newTag(mType)) {
            tag->load(stream);
            mStorage.push_back(std::move(tag));
        }
    }
}

void ListTag::merge(ListTag const& other) {
    if (other.size() == 0) { return; }
    if (mType == other.mType) {
        for (auto const& val : other.mStorage) {
            bool isEqual = false;
            for (const auto& tag : mStorage) {
                if (tag->equals(*val)) {
                    isEqual = true;
                    break;
                }
            }
            if (!isEqual) { push_back(val->copy()); }
        }
    } else {
        mType = other.mType;
        mStorage.clear();
        for (const auto& data : other.mStorage) { mStorage.emplace_back(data.get()->copy()); }
    }
}

void ListTag::push_back(std::unique_ptr<Tag>&& tag) {
    if (mStorage.empty()) { mType = tag->getType(); }
    mStorage.push_back(std::move(tag));
}

void ListTag::push_back(Tag const& tag) {
    if (mStorage.empty()) { mType = tag.getType(); }
    mStorage.push_back(tag.copy());
}

void ListTag::push_back(CompoundTagVariant val) { push_back(std::move(val).toUnique()); }

void ListTag::reserve(size_t size) { mStorage.reserve(size); }

bool ListTag::remove(size_t index) {
    if (index < mStorage.size()) {
        mStorage.erase(mStorage.begin() + static_cast<decltype(mStorage)::difference_type>(index));
        return true;
    }
    return false;
}

bool ListTag::remove(size_t startIndex, size_t endIndex) {
    if (startIndex < endIndex && endIndex < mStorage.size()) {
        mStorage.erase(
            mStorage.begin() + static_cast<decltype(mStorage)::difference_type>(startIndex),
            mStorage.begin() + static_cast<decltype(mStorage)::difference_type>(endIndex)
        );
        return true;
    }
    return false;
}

void ListTag::clear() noexcept { mStorage.clear(); }

ListTag::TagList&       ListTag::storage() noexcept { return mStorage; }
ListTag::TagList const& ListTag::storage() const noexcept { return mStorage; }

void ListTag::forEachCompoundTag(std::function<void(CompoundTag const& tag)> func) {
    if (mType == Type::Compound) {
        for (auto& tag : mStorage) { func(tag->as<CompoundTag>()); }
    }
}

size_t ListTag::size() const noexcept { return mStorage.size(); }
bool   ListTag::empty() const { return mStorage.empty(); }

Tag::Type const& ListTag::getElementType() const { return mType; }
Tag::Type&       ListTag::getElementType() { return mType; }

Tag&       ListTag::operator[](size_t index) noexcept { return *mStorage[index]; }
Tag const& ListTag::operator[](size_t index) const noexcept { return *mStorage[index]; }

Tag&       ListTag::at(size_t index) { return *mStorage.at(index); }
Tag const& ListTag::at(size_t index) const { return *mStorage.at(index); }

ListTag::iterator ListTag::begin() noexcept { return mStorage.begin(); }
ListTag::iterator ListTag::end() noexcept { return mStorage.end(); }

ListTag::reverse_iterator ListTag::rbegin() noexcept { return mStorage.rbegin(); }
ListTag::reverse_iterator ListTag::rend() noexcept { return mStorage.rend(); }

ListTag::const_iterator ListTag::begin() const noexcept { return cbegin(); }
ListTag::const_iterator ListTag::end() const noexcept { return cend(); }

ListTag::const_iterator ListTag::cbegin() const noexcept { return mStorage.cbegin(); }
ListTag::const_iterator ListTag::cend() const noexcept { return mStorage.cend(); }

ListTag::const_reverse_iterator ListTag::crbegin() const noexcept { return mStorage.crbegin(); }
ListTag::const_reverse_iterator ListTag::crend() const noexcept { return mStorage.crend(); }

ListTag::iterator ListTag::erase(const_iterator where) noexcept { return mStorage.erase(where); }
ListTag::iterator ListTag::erase(const_iterator first, const_iterator last) noexcept {
    return mStorage.erase(first, last);
}

bool ListTag::set(size_t index, Tag const& tag) {
    if (index < mStorage.size()) {
        mStorage[index] = tag.copy();
        return true;
    }
    return false;
}

} // namespace nbt