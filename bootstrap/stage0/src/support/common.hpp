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

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace phs {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using usize = std::size_t;

[[noreturn]] inline void fatal(const char* file, int line, const char* msg) {
    std::fprintf(stderr, "stage0 internal error: %s (%s:%d)\n", msg, file, line);
    std::abort();
}

}  // namespace phs

#define PHS_ASSERT(cond)                                         \
    do {                                                         \
        if (!(cond)) ::phs::fatal(__FILE__, __LINE__, #cond);    \
    } while (0)

#define PHS_UNREACHABLE() ::phs::fatal(__FILE__, __LINE__, "unreachable")
