// Copyright 2026 The Phantasos Authors
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include "support/common.hpp"

namespace phs {

// Decodes one UTF-8 encoded Unicode scalar value from p[0..n). Returns its
// length in bytes (1-4) and stores the value in *cp, or returns 0 if the bytes
// are not a valid shortest-form encoding (truncated, overlong, surrogate,
// above U+10FFFF, or a stray continuation byte).
inline u32 utf8_decode(const char* p, usize n, u32* cp) {
    if (n == 0) return 0;
    u8 b0 = static_cast<u8>(p[0]);
    if (b0 < 0x80) {
        *cp = b0;
        return 1;
    }
    u32 len;
    u32 value;
    u32 min;
    if ((b0 & 0xE0) == 0xC0) {
        len = 2;
        value = b0 & 0x1F;
        min = 0x80;
    } else if ((b0 & 0xF0) == 0xE0) {
        len = 3;
        value = b0 & 0x0F;
        min = 0x800;
    } else if ((b0 & 0xF8) == 0xF0) {
        len = 4;
        value = b0 & 0x07;
        min = 0x10000;
    } else {
        return 0;
    }
    if (n < len) return 0;
    for (u32 i = 1; i < len; ++i) {
        u8 b = static_cast<u8>(p[i]);
        if ((b & 0xC0) != 0x80) return 0;
        value = (value << 6) | (b & 0x3F);
    }
    if (value < min || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) return 0;
    *cp = value;
    return len;
}

// Number of bytes to advance past the character at p: the length of a valid
// sequence, or 1 for an invalid byte. Each step counts as one display column.
inline u32 utf8_step(const char* p, usize n) {
    u32 cp;
    u32 len = utf8_decode(p, n, &cp);
    return len ? len : 1;
}

}  // namespace phs
