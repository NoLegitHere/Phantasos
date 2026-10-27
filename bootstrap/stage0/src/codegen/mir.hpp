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
// strings are literals, passed around whole.

namespace phs::mir {

enum class Type : u8 { Unit, Never, Bool, I8, I16, I32, I64, U8, U16, U32, U64, Str };

std::string_view type_name(Type type);
bool is_signed(Type type);
u32 bit_width(Type type);  // 0 for Unit and Never, 1 for Bool, 128 for Str
u32 words(Type type);      // machine words: 0 for Unit and Never, 2 for Str, 1 otherwise

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
};

struct Inst {
    Op op;
    Reg dst;              // invalid for a call without a value
    Reg a, b;             // operands; comparisons use the operand type
    u64 imm = 0;          // Const: the value, sign-extended for signed types; Str: index into Program::strings
    u32 callee = 0;       // Call: function index
    std::vector<Reg> args;  // Call: arguments in parameter order
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

struct Program {
    std::vector<Function> functions;
    u32 main = 0;
    std::vector<std::string> strings;  // literal bytes, each distinct value once
};

// Lowers a file that passed check_codegen_subset. Evaluation is strictly left
// to right: operands, call arguments in source order, then compound
// assignment's target before its value. Unreachable blocks are removed.
// Expression lowering is iterative; control nesting follows the syntax, which
// the parser bounds (E0112).
[[nodiscard]] Program lower(const ast::File& file, const Resolution& resolution, const ScalarTypes& types,
                            const Interner& names);

[[nodiscard]] std::string dump(const Program& program);

}  // namespace phs::mir
