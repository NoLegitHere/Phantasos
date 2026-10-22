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

#include <string_view>
#include <vector>

#include "support/arena.hpp"

namespace phs {

// Interned string handle. Id 0 is the empty string.
struct Symbol {
    u32 id = 0;
    bool empty() const { return id == 0; }
    friend bool operator==(Symbol a, Symbol b) { return a.id == b.id; }
};

class Interner {
public:
    Interner();
    Symbol intern(std::string_view s);
    std::string_view str(Symbol s) const { return strings_[s.id]; }
    usize size() const { return strings_.size(); }

private:
    void grow();
    Arena arena_;
    std::vector<std::string_view> strings_;
    std::vector<u32> table_;  // open addressing; stores id+1, 0 = empty slot
};

}  // namespace phs
