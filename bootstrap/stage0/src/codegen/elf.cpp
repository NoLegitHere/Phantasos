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

#include "codegen/elf.hpp"

#include <format>
#include <initializer_list>
#include <limits>
#include <unordered_map>

namespace phs::elf {
namespace {

constexpr u64 kHeaderSize = 64, kProgramHeaderSize = 56, kSectionHeaderSize = 64, kSymbolSize = 24;
constexpr u64 kTextOffset = 128;  // after the ELF header and the program header, 16-byte aligned

class Writer {
public:
    std::vector<u8> bytes;

    void u8_(u8 v) { bytes.push_back(v); }
    void u16_(u16 v) { put(v, 2); }
    void u32_(u32 v) { put(v, 4); }
    void u64_(u64 v) { put(v, 8); }
    void align(u64 to, u8 fill = 0) {
        while (bytes.size() % to) bytes.push_back(fill);
    }

private:
    void put(u64 v, int size) {
        for (int i = 0; i < size; ++i) bytes.push_back(static_cast<u8>(v >> (8 * i)));
    }
};

struct Placed {
    const obj::Symbol* symbol;
    u64 address;
};

}  // namespace

std::expected<std::vector<u8>, std::string> link(std::span<const obj::Object> objects, std::string_view entry) {
    // Lay out .text: each object starts 16-byte aligned, padded with int3.
    std::vector<u8> text;
    std::vector<u64> bases;
    for (const auto& object : objects) {
        while (text.size() % 16) text.push_back(0xCC);
        bases.push_back(text.size());
        text.insert(text.end(), object.text.begin(), object.text.end());
    }
    const u64 text_address = kBaseAddress + kTextOffset;

    std::vector<Placed> placed;
    std::unordered_map<std::string_view, u64> addresses;
    for (usize o = 0; o < objects.size(); ++o)
        for (const auto& symbol : objects[o].symbols) {
            if (!symbol.defined) continue;
            const u64 address = text_address + bases[o] + symbol.offset;
            if (!addresses.emplace(symbol.name, address).second)
                return std::unexpected(std::format("symbol '{}' is defined more than once", symbol.name));
            placed.push_back({&symbol, address});
        }

    for (usize o = 0; o < objects.size(); ++o)
        for (const auto& reloc : objects[o].relocations) {
            const auto& name = objects[o].symbols[reloc.symbol].name;
            const auto found = addresses.find(name);
            if (found == addresses.end()) return std::unexpected(std::format("undefined symbol '{}'", name));
            const u64 field = bases[o] + reloc.offset;
            const i64 value = static_cast<i64>(found->second) + reloc.addend - static_cast<i64>(text_address + field);
            if (value < std::numeric_limits<i32>::min() || value > std::numeric_limits<i32>::max())
                return std::unexpected(std::format("reference to '{}' is out of rel32 range", name));
            for (int i = 0; i < 4; ++i) text[field + i] = static_cast<u8>(static_cast<u64>(value) >> (8 * i));
        }

    const auto start = addresses.find(entry);
    if (start == addresses.end()) return std::unexpected(std::format("undefined entry symbol '{}'", entry));

    // .strtab and .shstrtab
    std::string strtab(1, '\0');
    std::vector<u32> names;
    for (const auto& p : placed) {
        names.push_back(static_cast<u32>(strtab.size()));
        strtab += p.symbol->name;
        strtab += '\0';
    }
    const std::string shstrtab = std::string("\0.text\0.symtab\0.strtab\0.shstrtab\0", 33);
    constexpr u32 kTextName = 1, kSymtabName = 7, kStrtabName = 15, kShstrtabName = 23;

    const u64 load_size = kTextOffset + text.size();
    const u64 symtab_offset = (load_size + 7) / 8 * 8;
    const u64 symtab_size = kSymbolSize * (placed.size() + 1);
    const u64 strtab_offset = symtab_offset + symtab_size;
    const u64 shstrtab_offset = strtab_offset + strtab.size();
    const u64 section_offset = (shstrtab_offset + shstrtab.size() + 7) / 8 * 8;

    Writer w;
    // ELF header: 64-bit, little-endian, System V ABI, executable, x86_64.
    for (const u8 b : std::initializer_list<u8>{0x7F, 'E', 'L', 'F', 2, 1, 1, 0}) w.u8_(b);
    w.u64_(0);
    w.u16_(2);   // ET_EXEC
    w.u16_(62);  // EM_X86_64
    w.u32_(1);
    w.u64_(start->second);
    w.u64_(kHeaderSize);
    w.u64_(section_offset);
    w.u32_(0);
    w.u16_(kHeaderSize);
    w.u16_(kProgramHeaderSize);
    w.u16_(1);
    w.u16_(kSectionHeaderSize);
    w.u16_(5);
    w.u16_(4);
    // PT_LOAD covering the headers and .text, readable and executable.
    w.u32_(1);
    w.u32_(5);  // PF_R | PF_X
    w.u64_(0);
    w.u64_(kBaseAddress);
    w.u64_(kBaseAddress);
    w.u64_(load_size);
    w.u64_(load_size);
    w.u64_(0x1000);
    w.align(16);
    PHS_ASSERT(w.bytes.size() == kTextOffset);
    w.bytes.insert(w.bytes.end(), text.begin(), text.end());

    // .symtab: the null symbol, then every defined symbol as a global in .text.
    w.align(8);
    PHS_ASSERT(w.bytes.size() == symtab_offset);
    for (int i = 0; i < 3; ++i) w.u64_(0);
    for (usize i = 0; i < placed.size(); ++i) {
        w.u32_(names[i]);
        w.u8_(static_cast<u8>(1 << 4 | (placed[i].symbol->data ? 1 : 2)));  // STB_GLOBAL, STT_OBJECT or STT_FUNC
        w.u8_(0);
        w.u16_(1);  // .text
        w.u64_(placed[i].address);
        w.u64_(placed[i].symbol->size);
    }
    for (const char c : strtab) w.u8_(static_cast<u8>(c));
    for (const char c : shstrtab) w.u8_(static_cast<u8>(c));
    w.align(8);
    PHS_ASSERT(w.bytes.size() == section_offset);

    const auto section = [&](u32 name, u32 type, u64 flags, u64 address, u64 offset, u64 size, u32 link, u32 info,
                             u64 align, u64 entsize) {
        w.u32_(name);
        w.u32_(type);
        w.u64_(flags);
        w.u64_(address);
        w.u64_(offset);
        w.u64_(size);
        w.u32_(link);
        w.u32_(info);
        w.u64_(align);
        w.u64_(entsize);
    };
    section(0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    section(kTextName, 1, 6, text_address, kTextOffset, text.size(), 0, 0, 16, 0);  // SHF_ALLOC | SHF_EXECINSTR
    section(kSymtabName, 2, 0, 0, symtab_offset, symtab_size, 3, 1, 8, kSymbolSize);
    section(kStrtabName, 3, 0, 0, strtab_offset, strtab.size(), 0, 0, 1, 0);
    section(kShstrtabName, 3, 0, 0, shstrtab_offset, shstrtab.size(), 0, 0, 1, 0);
    return std::move(w.bytes);
}

}  // namespace phs::elf
