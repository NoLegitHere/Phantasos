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

#include <concepts>
#include <limits>
#include <utility>
#include <vector>

#include "support/common.hpp"

namespace phs {

// Mixes an integer key so that dense ids spread over a power-of-two table.
// std::hash is the identity on libstdc++ and byte-wise FNV-1a on MSVC; this
// is one multiply-xorshift round (the murmur3 finalizer) on both.
constexpr u64 mix_key(u64 key) {
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53ULL;
    key ^= key >> 33;
    return key;
}

// An open-addressing hash map from unsigned integers to values: one flat
// array of (key, value) slots with linear probing, so a lookup touches one
// or two cache lines and an insert never allocates a node. The largest key
// value marks an empty slot and cannot be stored.
//
// Unlike std::unordered_map, inserting may move every value, so references
// and iterators are invalidated by insertion; erase() also moves entries.
// Iteration order follows the slots and is deterministic for a given
// sequence of operations.
template <std::unsigned_integral K, typename V>
class FlatMap {
public:
    using value_type = std::pair<K, V>;
    static constexpr K kEmpty = std::numeric_limits<K>::max();

    template <typename Slot>
    class Iterator {
    public:
        Iterator(Slot* slot, Slot* last) : slot_(slot), last_(last) { skip(); }
        Slot& operator*() const { return *slot_; }
        Slot* operator->() const { return slot_; }
        Iterator& operator++() { ++slot_; skip(); return *this; }
        friend bool operator==(const Iterator& a, const Iterator& b) { return a.slot_ == b.slot_; }
    private:
        void skip() { while (slot_ != last_ && slot_->first == kEmpty) ++slot_; }
        Slot* slot_;
        Slot* last_;
    };
    using iterator = Iterator<value_type>;
    using const_iterator = Iterator<const value_type>;

    iterator begin() { return {slots_.data(), slots_.data() + slots_.size()}; }
    iterator end() { return {slots_.data() + slots_.size(), slots_.data() + slots_.size()}; }
    const_iterator begin() const { return {slots_.data(), slots_.data() + slots_.size()}; }
    const_iterator end() const { return {slots_.data() + slots_.size(), slots_.data() + slots_.size()}; }

    usize size() const { return size_; }
    bool empty() const { return size_ == 0; }
    bool contains(K key) const { return probe(key).second; }
    // Keeps the slot array, so a reused map does not allocate again.
    void clear() {
        if (!size_) return;
        for (auto& slot : slots_) slot = value_type{kEmpty, V{}};
        size_ = 0;
    }
    void reserve(usize count) {
        usize capacity = 16;
        while (capacity * 3 < count * 4 + 4) capacity *= 2;  // load factor stays under 3/4
        if (capacity > slots_.size()) rehash(capacity);
    }

    iterator find(K key) {
        const auto [index, found] = probe(key);
        return found ? at_slot(index) : end();
    }
    const_iterator find(K key) const {
        const auto [index, found] = probe(key);
        return found ? const_iterator{slots_.data() + index, slots_.data() + slots_.size()} : end();
    }
    V& at(K key) {
        const auto [index, found] = probe(key);
        PHS_ASSERT(found);
        return slots_[index].second;
    }
    const V& at(K key) const {
        const auto [index, found] = probe(key);
        PHS_ASSERT(found);
        return slots_[index].second;
    }

    template <typename... Args>
    std::pair<iterator, bool> try_emplace(K key, Args&&... args) {
        PHS_ASSERT(key != kEmpty);
        if ((size_ + 1) * 4 > slots_.size() * 3) rehash(slots_.empty() ? 16 : slots_.size() * 2);
        const auto [index, found] = probe(key);
        if (found) return {at_slot(index), false};
        slots_[index].first = key;
        slots_[index].second = V(std::forward<Args>(args)...);
        ++size_;
        return {at_slot(index), true};
    }
    template <typename... Args>
    std::pair<iterator, bool> emplace(K key, Args&&... args) { return try_emplace(key, std::forward<Args>(args)...); }
    V& operator[](K key) { return try_emplace(key).first->second; }

    // Backward-shift deletion: no tombstones, so probes stay short.
    usize erase(K key) {
        auto [index, found] = probe(key);
        if (!found) return 0;
        const usize mask = slots_.size() - 1;
        for (usize next = (index + 1) & mask; slots_[next].first != kEmpty; next = (next + 1) & mask) {
            const usize home = mix_key(slots_[next].first) & mask;
            // Move the entry back if its home is not cyclically in (index, next].
            if (((next - home) & mask) >= ((next - index) & mask)) {
                slots_[index] = std::move(slots_[next]);
                index = next;
            }
        }
        slots_[index] = value_type{kEmpty, V{}};
        --size_;
        return 1;
    }

private:
    // The slot holding key, or the empty slot where it would be inserted.
    std::pair<usize, bool> probe(K key) const {
        if (slots_.empty()) return {0, false};
        const usize mask = slots_.size() - 1;
        for (usize index = mix_key(key) & mask;; index = (index + 1) & mask) {
            if (slots_[index].first == key) return {index, true};
            if (slots_[index].first == kEmpty) return {index, false};
        }
    }
    iterator at_slot(usize index) { return {slots_.data() + index, slots_.data() + slots_.size()}; }
    void rehash(usize capacity) {
        std::vector<value_type> old(capacity, value_type{kEmpty, V{}});
        old.swap(slots_);
        const usize mask = capacity - 1;
        for (auto& slot : old) {
            if (slot.first == kEmpty) continue;
            usize index = mix_key(slot.first) & mask;
            while (slots_[index].first != kEmpty) index = (index + 1) & mask;
            slots_[index] = std::move(slot);
        }
    }

    std::vector<value_type> slots_;  // size is zero or a power of two
    usize size_ = 0;
};

// A FlatMap used as a set.
template <std::unsigned_integral K>
class FlatSet {
public:
    bool insert(K key) { return map_.try_emplace(key).second; }
    bool contains(K key) const { return map_.contains(key); }
    usize erase(K key) { return map_.erase(key); }
    void clear() { map_.clear(); }
    void reserve(usize count) { map_.reserve(count); }
    usize size() const { return map_.size(); }
    bool empty() const { return map_.empty(); }
private:
    struct Empty {};
    FlatMap<K, Empty> map_;
};

}  // namespace phs
