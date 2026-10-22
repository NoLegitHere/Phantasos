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

#include "support/interner.hpp"

namespace phs {

namespace {
u64 hash_str(std::string_view s) {
    u64 h = 1469598103934665603ull;  // FNV-1a
    for (char c : s) {
        h ^= static_cast<u8>(c);
        h *= 1099511628211ull;
    }
    return h;
}
}  // namespace

Interner::Interner() {
    table_.assign(1024, 0);
    strings_.push_back({});
}

Symbol Interner::intern(std::string_view s) {
    if (s.empty()) return {};
    if ((strings_.size() + 1) * 2 > table_.size()) grow();
    usize mask = table_.size() - 1;
    usize i = hash_str(s) & mask;
    while (true) {
        u32 slot = table_[i];
        if (slot == 0) {
            u32 id = static_cast<u32>(strings_.size());
            strings_.push_back(arena_.copy_str(s));
            table_[i] = id + 1;
            return {id};
        }
        if (strings_[slot - 1] == s) return {slot - 1};
        i = (i + 1) & mask;
    }
}

void Interner::grow() {
    std::vector<u32> next(table_.size() * 2, 0);
    usize mask = next.size() - 1;
    for (u32 slot : table_) {
        if (slot == 0) continue;
        usize i = hash_str(strings_[slot - 1]) & mask;
        while (next[i] != 0) i = (i + 1) & mask;
        next[i] = slot;
    }
    table_ = std::move(next);
}

}  // namespace phs
