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

#include <string>
#include <vector>

#include "support/common.hpp"

// A minimal relocatable object: one code section, its symbols and its
// relocations. Object and executable writers consume it; keeping relocations
// symbolic lets the same code be linked statically or loaded in memory.

namespace phs::obj {

struct Symbol {
    std::string name;
    bool defined = false;  // false: resolved by the linker (runtime entry points)
    u64 offset = 0;        // within .text when defined
    u64 size = 0;
};

// A 32-bit PC-relative reference: the linker writes S + A - P at offset,
// where P is the address of the field (ELF R_X86_64_PLT32 and PC32).
struct Relocation {
    u64 offset = 0;
    u32 symbol = 0;
    i64 addend = -4;
};

struct Object {
    std::vector<u8> text;
    std::vector<Symbol> symbols;
    std::vector<Relocation> relocations;
};

}  // namespace phs::obj
