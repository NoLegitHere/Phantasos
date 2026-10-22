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

#include "support/arena.hpp"

#include <cstdlib>

namespace phs {

namespace {
constexpr usize kChunkSize = 64 * 1024;
}

Arena::~Arena() {
    Chunk* c = head_;
    while (c) {
        Chunk* prev = c->prev;
        std::free(c);
        c = prev;
    }
}

void* Arena::alloc(usize size, usize align) {
    PHS_ASSERT(align != 0 && (align & (align - 1)) == 0);
    auto aligned = [&](char* p) {
        auto v = reinterpret_cast<std::uintptr_t>(p);
        return reinterpret_cast<char*>((v + align - 1) & ~(std::uintptr_t)(align - 1));
    };
    char* p = cur_ ? aligned(cur_) : nullptr;
    if (!p || p + size > end_) {
        usize need = size + align + sizeof(Chunk);
        usize chunk = need > kChunkSize ? need : kChunkSize;
        auto* c = static_cast<Chunk*>(std::malloc(chunk));
        if (!c) fatal(__FILE__, __LINE__, "out of memory");
        c->prev = head_;
        c->size = chunk;
        head_ = c;
        cur_ = reinterpret_cast<char*>(c + 1);
        end_ = reinterpret_cast<char*>(c) + chunk;
        p = aligned(cur_);
    }
    cur_ = p + size;
    used_ += size;
    return p;
}

}  // namespace phs
