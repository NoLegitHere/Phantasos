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

#include <format>
#include <limits>
#include <string>
#include <vector>

#include "codegen/mir.hpp"
#include "test_support.hpp"

namespace phs::test {
using mir::Op;
using mir::Term;
using mir::Type;

// A reference interpreter for MIR semantics: checked operations trap. Stage0
// strings are literals passed whole, so a String value is its index in
// Program::strings. An aggregate value is an index into a pool of immutable
// field lists, which Copy and Call share freely.
struct Outcome {
    bool trapped = false;
    u64 value = 0;
    u32 trap = 0;  // the x86_64 trap code: 1 for overflow, 2 for division by zero
    std::string output;  // what print wrote to standard output
};

inline u64 normalize(Type type, u64 bits) {
    const u32 width = mir::bit_width(type);
    if (width == 1) return bits & 1;
    if (width == 0 || width == 64) return bits;
    const u64 mask = (u64{1} << width) - 1;
    bits &= mask;
    if (mir::is_signed(type) && (bits >> (width - 1)) & 1) bits |= ~mask;
    return bits;
}

inline bool fits(Type type, u64 bits) { return normalize(type, bits) == bits; }

class Interpreter {
public:
    explicit Interpreter(const mir::Program& program) : program_(program) {}

    Outcome run() { return run(program_.main); }
    Outcome run(u32 function) {
        output_.clear();
        aggregates_.clear();
        auto outcome = call(function, {});
        outcome.output = output_;
        return outcome;
    }

private:
    Outcome call(u32 index, std::vector<u64> args) {
        CHECK(++depth_ < 10000);
        const auto& fun = program_.functions[index];
        std::vector<u64> regs(fun.regs.size());
        for (usize i = 0; i < args.size(); ++i) regs[i] = args[i];
        u32 block = 0;
        for (u64 steps = 0;; ++steps) {
            CHECK(steps < 10'000'000);
            const auto& b = fun.blocks[block];
            for (const auto& inst : b.insts) {
                if (inst.op == Op::Call) {
                    std::vector<u64> values;
                    for (const auto reg : inst.args) values.push_back(regs[reg.id]);
                    const auto result = call(inst.callee, std::move(values));
                    if (result.trapped) return result;
                    if (inst.dst.valid()) regs[inst.dst.id] = result.value;
                    continue;
                }
                if (inst.op == Op::Make) {
                    const Type type = fun.regs[inst.dst.id];
                    CHECK(mir::is_aggregate(type));
                    const auto& layout = program_.layouts[mir::layout_index(type)];
                    CHECK(inst.args.size() == layout.fields.size());
                    std::vector<u64> fields;
                    for (usize i = 0; i < inst.args.size(); ++i) {
                        CHECK(fun.regs[inst.args[i].id] == layout.fields[i]);
                        fields.push_back(regs[inst.args[i].id]);
                    }
                    aggregates_.push_back(std::move(fields));
                    regs[inst.dst.id] = aggregates_.size() - 1;
                    continue;
                }
                if (inst.op == Op::Field) {
                    const Type type = fun.regs[inst.a.id];
                    CHECK(mir::is_aggregate(type));
                    CHECK(fun.regs[inst.dst.id] == program_.layouts[mir::layout_index(type)].fields[inst.imm]);
                    regs[inst.dst.id] = aggregates_[regs[inst.a.id]][inst.imm];
                    continue;
                }
                if (inst.op == Op::With) {
                    const Type type = fun.regs[inst.a.id];
                    CHECK(mir::is_aggregate(type) && fun.regs[inst.dst.id] == type);
                    CHECK(fun.regs[inst.b.id] == program_.layouts[mir::layout_index(type)].fields[inst.imm]);
                    auto fields = aggregates_[regs[inst.a.id]];
                    fields[inst.imm] = regs[inst.b.id];
                    aggregates_.push_back(std::move(fields));
                    regs[inst.dst.id] = aggregates_.size() - 1;
                    continue;
                }
                if (inst.op == Op::Print) {
                    const Type type = fun.regs[inst.a.id];
                    const u64 value = regs[inst.a.id];
                    if (type == Type::Str) output_ += program_.strings[value];
                    else if (type == Type::Bool) output_ += value ? "true" : "false";
                    else if (mir::is_signed(type)) output_ += std::format("{}", static_cast<i64>(value));
                    else output_ += std::format("{}", value);
                    output_ += '\n';
                    continue;
                }
                const Type type = fun.regs[inst.dst.id];
                const Type operand = inst.a.valid() ? fun.regs[inst.a.id] : type;
                const u64 a = inst.a.valid() ? regs[inst.a.id] : 0, b2 = inst.b.valid() ? regs[inst.b.id] : 0;
                const bool sign = mir::is_signed(operand);
                const auto sa = static_cast<i64>(a), sb = static_cast<i64>(b2);
                u64 out = 0;
                u32 trap = 0;
                switch (inst.op) {
                    case Op::Const: out = normalize(type, inst.imm); CHECK(out == inst.imm); break;
                    case Op::Copy: out = a; break;
                    case Op::Str: out = inst.imm; CHECK(inst.imm < program_.strings.size()); break;
                    case Op::Neg: {
                        i64 r = 0;
                        trap = __builtin_sub_overflow(i64{0}, sa, &r) || !fits(type, static_cast<u64>(r));
                        out = static_cast<u64>(r);
                        break;
                    }
                    case Op::Not: out = !a; break;
                    case Op::BitNot: out = normalize(type, ~a); break;
                    case Op::Add: case Op::Sub: case Op::Mul: {
                        if (sign) {
                            i64 r = 0;
                            trap = inst.op == Op::Add ? __builtin_add_overflow(sa, sb, &r)
                                 : inst.op == Op::Sub ? __builtin_sub_overflow(sa, sb, &r)
                                                      : __builtin_mul_overflow(sa, sb, &r);
                            out = static_cast<u64>(r);
                        } else {
                            trap = inst.op == Op::Add ? __builtin_add_overflow(a, b2, &out)
                                 : inst.op == Op::Sub ? __builtin_sub_overflow(a, b2, &out)
                                                      : __builtin_mul_overflow(a, b2, &out);
                        }
                        trap = trap || !fits(type, out);
                        break;
                    }
                    case Op::Div: case Op::Rem: {
                        const u32 width = mir::bit_width(type);
                        const i64 min = width == 64 ? std::numeric_limits<i64>::min() : -(i64{1} << (width - 1));
                        trap = b2 == 0 ? 2 : sign && sb == -1 && sa == min ? 1 : 0;
                        if (!trap)
                            out = sign ? static_cast<u64>(inst.op == Op::Div ? sa / sb : sa % sb)
                                       : inst.op == Op::Div ? a / b2 : a % b2;
                        break;
                    }
                    case Op::WrapAdd: out = normalize(type, a + b2); break;
                    case Op::WrapSub: out = normalize(type, a - b2); break;
                    case Op::WrapMul: out = normalize(type, a * b2); break;
                    case Op::And: out = a & b2; break;
                    case Op::Or: out = a | b2; break;
                    case Op::Xor: out = a ^ b2; break;
                    case Op::Eq: out = a == b2; break;
                    case Op::Ne: out = a != b2; break;
                    case Op::Lt: out = sign ? sa < sb : a < b2; break;
                    case Op::Le: out = sign ? sa <= sb : a <= b2; break;
                    case Op::Gt: out = sign ? sa > sb : a > b2; break;
                    case Op::Ge: out = sign ? sa >= sb : a >= b2; break;
                    case Op::Widen: out = a; CHECK(fits(type, a)); break;
                    case Op::Truncate: out = normalize(type, a); break;
                    case Op::Call: case Op::Print: case Op::Make: case Op::Field: case Op::With: break;
                }
                if (trap) return {true, 0, trap};
                regs[inst.dst.id] = out;
            }
            switch (b.term) {
                case Term::Br: block = b.target; break;
                case Term::CondBr: block = regs[b.value.id] ? b.target : b.other; break;
                case Term::Ret:
                    --depth_;
                    return {false, b.value.valid() ? regs[b.value.id] : 0};
                case Term::Unreachable: CHECK(false && "reached unreachable");
            }
        }
    }

    const mir::Program& program_;
    usize depth_ = 0;
    std::string output_;
    std::vector<std::vector<u64>> aggregates_;
};


}  // namespace phs::test
