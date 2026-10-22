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

#include <cstring>
#include <new>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "support/common.hpp"

namespace phs {

// Bump allocator. Objects allocated here are never destroyed individually, so
// only trivially destructible types may be placed in it.
class Arena {
public:
    Arena() = default;
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    ~Arena();

    void* alloc(usize size, usize align);

    template <typename T, typename... Args>
    T* make(Args&&... args) {
        static_assert(std::is_trivially_destructible_v<T>, "arena objects must be trivially destructible");
        void* p = alloc(sizeof(T), alignof(T));
        return new (p) T(std::forward<Args>(args)...);
    }

    template <typename T>
    std::span<T> copy(const std::vector<T>& v) {
        static_assert(std::is_trivially_destructible_v<T>);
        if (v.empty()) return {};
        T* p = static_cast<T*>(alloc(sizeof(T) * v.size(), alignof(T)));
        for (usize i = 0; i < v.size(); ++i) new (p + i) T(v[i]);
        return {p, v.size()};
    }

    std::string_view copy_str(std::string_view s) {
        if (s.empty()) return {};
        char* p = static_cast<char*>(alloc(s.size(), 1));
        std::memcpy(p, s.data(), s.size());
        return {p, s.size()};
    }

    usize bytes_used() const { return used_; }

private:
    struct Chunk {
        Chunk* prev;
        usize size;
    };
    Chunk* head_ = nullptr;
    char* cur_ = nullptr;
    char* end_ = nullptr;
    usize used_ = 0;
};

}  // namespace phs
