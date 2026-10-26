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
#include <string_view>

#include "codegen/mir.hpp"
#include "codegen/object.hpp"

// Naive x86_64 code generation from MIR. Every register lives in a stack slot
// below rbp and holds its value normalized to 64 bits (sign-extended for
// signed types, zero-extended otherwise). Each instruction loads its operands
// into rax and rcx, computes, and stores the result. Calls follow the System V
// AMD64 convention: integer arguments in rdi, rsi, rdx, rcx, r8 and r9, the
// rest on the stack, and the result in rax.
//
// A failed check calls the runtime's trap entry with a Trap code in edi; the
// entry does not return.

namespace phs::x86_64 {

enum class Trap : u32 { Overflow = 1, DivideByZero = 2 };

inline constexpr std::string_view kTrapSymbol = "__phantasos_trap";

// "_P" followed by the length-prefixed MIR name (abi.md A.7, without a package).
[[nodiscard]] std::string symbol_name(const mir::Function& function);

// Symbols 0..n-1 are the program's functions in order; the trap entry follows.
// When listing is non-null it receives a readable assembly listing.
[[nodiscard]] obj::Object emit(const mir::Program& program, std::string* listing = nullptr);

}  // namespace phs::x86_64
