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

#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "sema/types.hpp"

// MIR: the typed, block-structured IR between the checked AST and machine
// code. Registers are typed but not SSA: a local keeps one register that every
// assignment writes. Arithmetic is checked by default; a checked operation
// traps on overflow, and division and remainder also trap on a zero divisor.
// A String is two words, a pointer to its UTF-8 bytes and their length; stage0
// strings are literals, passed around whole. A struct is an aggregate value
// with a Layout (abi.md A.2); aggregates are built whole and read one field at
// a time.

namespace phs::mir {

// Values from Aggregate up name Program::layouts[type - Aggregate].
enum class Type : u32 { Unit, Never, Bool, I8, I16, I32, I64, U8, U16, U32, U64, Str, Aggregate = 16 };

constexpr bool is_aggregate(Type type) { return type >= Type::Aggregate; }
constexpr Type aggregate(u32 layout) { return static_cast<Type>(static_cast<u32>(Type::Aggregate) + layout); }
constexpr u32 layout_index(Type type) { return static_cast<u32>(type) - static_cast<u32>(Type::Aggregate); }

bool is_signed(Type type);
u32 bit_width(Type type);  // 0 for Unit, Never and aggregates, 1 for Bool, 128 for Str

struct Reg {
    u32 id = std::numeric_limits<u32>::max();
    bool valid() const { return id != std::numeric_limits<u32>::max(); }
    friend bool operator==(Reg, Reg) = default;
};

enum class Op : u8 {
    Const, Copy, Str,
    Neg, Not, BitNot,
    Add, Sub, Mul, Div, Rem, WrapAdd, WrapSub, WrapMul, And, Or, Xor,
    Eq, Ne, Lt, Le, Gt, Ge,
    Widen, Truncate,
    Call,
    Print,  // writes a's value and a newline to standard output
    Make,   // an aggregate from args, its fields in declaration order
    Field,  // field imm of the aggregate a
};

struct Inst {
    Op op;
    Reg dst;              // invalid for a call without a value and for print
    Reg a, b;             // operands; comparisons use the operand type
    u64 imm = 0;          // Const: the value, sign-extended for signed types; Str: index into Program::strings;
                          // Field: the field's declaration index
    u32 callee = 0;       // Call: function index
    std::vector<Reg> args;  // Call: arguments in parameter order; Make: field values
};

enum class Term : u8 { Br, CondBr, Ret, Unreachable };

struct Block {
    std::vector<Inst> insts;
    Term term = Term::Unreachable;
    Reg value;                // CondBr: the Bool condition; Ret: the result, if any
    u32 target = 0, other = 0;  // Br: target; CondBr: target if true, other if false
};

struct Function {
    std::string name;  // unique within the program
    Span span;
    std::vector<Type> regs;  // register types; parameters come first
    u32 params = 0;
    Type result = Type::Unit;
    std::vector<Block> blocks;  // blocks[0] is the entry; every block is reachable
};

// Fields keep declaration order; offsets place them by decreasing alignment,
// stable among equal alignments, and the size is a multiple of the alignment.
struct Layout {
    std::string name;
    std::vector<Type> fields;
    std::vector<std::string> names;
    std::vector<u32> offsets;
    u32 size = 0, align = 1;
};

struct Program {
    std::vector<Function> functions;
    u32 main = 0;
    std::vector<std::string> strings;  // literal bytes, each distinct value once
    std::vector<Layout> layouts;       // each struct type once
};

std::string type_name(const Program& program, Type type);
u32 size_of(const Program& program, Type type);   // bytes: 0 for Unit and Never, 1 for Bool
u32 align_of(const Program& program, Type type);
u32 words(const Program& program, Type type);     // 8-byte words: 0 for Unit and Never
// Fills in a layout's offsets, size and alignment from its field types.
void place_fields(const Program& program, Layout& layout);

// Lowers a file that passed check_codegen_subset. Evaluation is strictly left
// to right: operands, call arguments in source order, then compound
// assignment's target before its value. Unreachable blocks are removed.
// Expression lowering is iterative; control nesting follows the syntax, which
// the parser bounds (E0112).
[[nodiscard]] Program lower(const ast::File& file, const Resolution& resolution, const ScalarTypes& types,
                            const Interner& names);

[[nodiscard]] std::string dump(const Program& program);

}  // namespace phs::mir
