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

#include <cstdint>
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
    // Padding needed to align p, computed on integers so that no pointer is
    // ever formed outside the current chunk.
    auto padding_for = [&](const char* p) -> usize {
        auto v = reinterpret_cast<std::uintptr_t>(p);
        return static_cast<usize>((align - (v & (align - 1))) & (align - 1));
    };
    if (cur_) {
        usize avail = static_cast<usize>(end_ - cur_);
        usize pad = padding_for(cur_);
        if (pad <= avail && size <= avail - pad) {
            char* p = cur_ + pad;
            cur_ = p + size;
            used_ += size;
            return p;
        }
    }
    if (size > SIZE_MAX - align - sizeof(Chunk)) fatal(__FILE__, __LINE__, "allocation too large");
    usize need = sizeof(Chunk) + align + size;
    usize chunk = need > kChunkSize ? need : kChunkSize;
    auto* c = static_cast<Chunk*>(std::malloc(chunk));
    if (!c) fatal(__FILE__, __LINE__, "out of memory");
    c->prev = head_;
    c->size = chunk;
    head_ = c;
    char* base = reinterpret_cast<char*>(c + 1);
    end_ = reinterpret_cast<char*>(c) + chunk;
    char* p = base + padding_for(base);  // pad < align, so p + size <= end_ by construction
    cur_ = p + size;
    used_ += size;
    return p;
}

}  // namespace phs
