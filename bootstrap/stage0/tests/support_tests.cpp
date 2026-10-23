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

#include "test_support.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "support/arena.hpp"
#include "support/interner.hpp"
#include "support/scoped_value.hpp"
#include "support/source.hpp"

using namespace phs;
using phs::test::TempDirectory;

namespace {

template <typename T, typename... Args>
concept CanMake = requires(Arena& arena, Args&&... args) { arena.make<T>(std::forward<Args>(args)...); };

template <typename R>
concept CanCopy = requires(Arena& arena, const R& range) { arena.copy(range); };

struct MoveOnly {
    MoveOnly() = default;
    MoveOnly(MoveOnly&&) = default;
    MoveOnly(const MoveOnly&) = delete;
};

struct MutableRange {
    int* begin();
    int* end();
};

struct alignas(128) Aligned { int value; };

static_assert(CanMake<int, int> && CanMake<Aligned, int>);
static_assert(!CanMake<std::string> && !CanMake<int, std::string>);
static_assert(!CanMake<const int> && !CanMake<int[2]>);
static_assert(CanCopy<std::array<int, 3>> && CanCopy<std::span<const int>>);
static_assert(!CanCopy<std::vector<std::string>> && !CanCopy<std::vector<MoveOnly>>);
static_assert(!CanCopy<MutableRange> && !CanCopy<std::vector<bool>>);
static_assert(!CanCopy<std::ranges::subrange<int*, std::unreachable_sentinel_t>>);
static_assert(!std::convertible_to<u32, FileId> && !std::convertible_to<FileId, u32>);
static_assert(SourceLimits::kMaxFileBytes < std::numeric_limits<u32>::max());

void write_file(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    CHECK(out);
}

void source_limits_and_loading() {
    TempDirectory temp;
    const std::string raw = "\xEF\xBB\xBF" "a\r\n\xCE\xB2\r";
    CHECK(raw.size() == 9);
    const auto path = temp.path / "source.phs";
    write_file(path, raw);

    SourceManager limited({.max_file_bytes = 8, .max_files = 2});
    const auto added = limited.add("raw", raw);
    const auto loaded = limited.load(path);
    CHECK(!added && !loaded);
    CHECK(added.error().kind == SourceErrorKind::TooLarge && added.error().limit == 8);
    CHECK(loaded.error().kind == SourceErrorKind::TooLarge && loaded.error().limit == 8);
    CHECK(loaded.error().message().find("8 bytes") != std::string::npos);

    const auto missing = limited.load(temp.path / "missing.phs");
    CHECK(!missing && missing.error().kind == SourceErrorKind::Metadata && missing.error().cause);
    const auto directory = limited.load(temp.path);
    CHECK(!directory && directory.error().kind == SourceErrorKind::NotRegularFile);
    const auto empty = limited.add("empty", "");
    CHECK(empty && empty->value == 0);  // Failed operations did not consume IDs.
    const auto exact = limited.add("exact", "12345678");
    CHECK(exact && exact->value == 1);
    const auto excess = limited.add("third", "");
    CHECK(!excess && excess.error().kind == SourceErrorKind::TooManyFiles && excess.error().limit == 2);
    write_file(path, "");
    const auto excess_load = limited.load(path);
    CHECK(!excess_load && excess_load.error().kind == SourceErrorKind::TooManyFiles);

    SourceManager exact_limit({.max_file_bytes = 9});
    write_file(path, raw);
    const auto normalized = exact_limit.load(path);
    CHECK(normalized && exact_limit.file(*normalized).text == "a\n\xCE\xB2\n");
    CHECK(exact_limit.file(*normalized).line_starts == std::vector<u32>({0, 2, 5}));
    write_file(path, "");
    SourceManager zero_bytes({.max_file_bytes = 0});
    const auto empty_load = zero_bytes.load(path);
    CHECK(empty_load && zero_bytes.file(*empty_load).text.empty());
    CHECK(zero_bytes.line_col(*empty_load, 0).col == 1);
    CHECK(!zero_bytes.add("one", "x"));
    SourceManager zero_files({.max_files = 0});
    CHECK(!zero_files.add("empty", ""));
}

void source_positions_and_lifetimes() {
    SourceManager sources;
    const auto id = sources.add("unicode", "\t\xC3\xA9\xF0\x9F\x98\x80\nz");
    CHECK(id);
    CHECK(sources.line_col(*id, 3).line == 1 && sources.line_col(*id, 3).col == 3);
    CHECK(sources.line_col(*id, 7).col == 4);
    CHECK(sources.line_col(*id, 8).line == 2 && sources.line_col(*id, 8).col == 1);
    CHECK(sources.line_col(*id, 9).col == 2);
    CHECK(sources.line_col(*id, 100).col == 2);
    CHECK(sources.line_text(*id, 2) == "z");
    CHECK(sources.line_text(*id, 0).empty() && sources.line_text(*id, 3).empty());
    const SourceFile* first = &sources.file(*id);
    const std::string_view view = first->text;
    for (int i = 0; i < 1024; ++i) CHECK(sources.add("more", "abc"));
    CHECK(&sources.file(*id) == first);
    CHECK(view == sources.file(*id).text && view.data() == sources.file(*id).text.data());
    const Span joined = Span::join({*id, 8, 9}, {*id, 1, 3});
    CHECK(joined.file == *id && joined.lo == 1 && joined.hi == 9);
}

void arena_storage() {
    Arena arena;
    const auto* aligned = arena.make<Aligned>(42);
    CHECK(aligned->value == 42 && reinterpret_cast<std::uintptr_t>(aligned) % alignof(Aligned) == 0);
    std::array<Aligned, 2> values{Aligned{1}, Aligned{2}};
    auto copy = arena.copy(values);
    values[0].value = 99;
    CHECK(copy.size() == 2 && copy[0].value == 1 && copy[1].value == 2);
    CHECK(reinterpret_cast<std::uintptr_t>(copy.data()) % alignof(Aligned) == 0);
    CHECK(arena.copy(std::span<const int>{}).empty());
    CHECK(arena.copy_str({}).empty());
    std::string original = "interned data";
    const auto stored = arena.copy_str(original);
    original[0] = 'X';
    for (int i = 0; i < 128; ++i) CHECK(arena.copy_str(std::string(4096, 'a')).size() == 4096);
    CHECK(stored == "interned data" && copy[1].value == 2 && aligned->value == 42);
}

void interner_and_scoped_state() {
    Interner interner;
    CHECK(interner.intern("").empty());
    const Symbol id = interner.intern("same");
    const auto view = interner.str(id);
    for (int i = 0; i < 1024; ++i) interner.intern(std::format("symbol{}", i));
    CHECK(interner.intern("same") == id && interner.str(id) == view);
    CHECK(interner.str(id).data() == view.data());
    int state = 1;
    const auto early_return = [&] {
        const ScopedValue outer(state, 2);
        {
            const ScopedValue inner(state, 3);
            CHECK(state == 3);
        }
        CHECK(state == 2);
        return;
    };
    early_return();
    CHECK(state == 1);
}

}  // namespace

int main() {
    source_limits_and_loading();
    source_positions_and_lifetimes();
    arena_storage();
    interner_and_scoped_state();
}
