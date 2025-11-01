// Copyright © 2025 GlacieTeam. All rights reserved.
//
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was not
// distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once
#include <cstdint>

namespace nbt::literals {

constexpr uint8_t operator""_b(unsigned long long value) noexcept { return static_cast<uint8_t>(value); }

constexpr short operator""_s(unsigned long long value) noexcept { return static_cast<short>(value); }

constexpr int64_t operator""_l(unsigned long long value) noexcept { return static_cast<int64_t>(value); }

} // namespace nbt::literals
