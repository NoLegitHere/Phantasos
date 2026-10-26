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

#include "codegen/mir.hpp"

#include <format>

namespace phs::mir {

std::string_view type_name(Type type) {
    switch (type) {
        case Type::Unit: return "unit";
        case Type::Never: return "never";
        case Type::Bool: return "bool";
        case Type::I8: return "i8";
        case Type::I16: return "i16";
        case Type::I32: return "i32";
        case Type::I64: return "i64";
        case Type::U8: return "u8";
        case Type::U16: return "u16";
        case Type::U32: return "u32";
        case Type::U64: return "u64";
    }
    unreachable();
}

bool is_signed(Type type) { return type >= Type::I8 && type <= Type::I64; }

u32 bit_width(Type type) {
    switch (type) {
        case Type::Unit: case Type::Never: return 0;
        case Type::Bool: return 1;
        case Type::I8: case Type::U8: return 8;
        case Type::I16: case Type::U16: return 16;
        case Type::I32: case Type::U32: return 32;
        case Type::I64: case Type::U64: return 64;
    }
    unreachable();
}

namespace {

std::string_view op_name(Op op) {
    switch (op) {
        case Op::Const: return "const";
        case Op::Copy: return "copy";
        case Op::Neg: return "neg";
        case Op::Not: return "not";
        case Op::BitNot: return "bitnot";
        case Op::Add: return "add";
        case Op::Sub: return "sub";
        case Op::Mul: return "mul";
        case Op::Div: return "div";
        case Op::Rem: return "rem";
        case Op::WrapAdd: return "wadd";
        case Op::WrapSub: return "wsub";
        case Op::WrapMul: return "wmul";
        case Op::And: return "and";
        case Op::Or: return "or";
        case Op::Xor: return "xor";
        case Op::Eq: return "eq";
        case Op::Ne: return "ne";
        case Op::Lt: return "lt";
        case Op::Le: return "le";
        case Op::Gt: return "gt";
        case Op::Ge: return "ge";
        case Op::Widen: return "widen";
        case Op::Truncate: return "truncate";
        case Op::Call: return "call";
    }
    unreachable();
}

// Constants print as the signed or unsigned value of their type.
std::string constant(Type type, u64 bits) {
    if (type == Type::Bool) return bits ? "true" : "false";
    if (is_signed(type)) return std::format("{}", static_cast<i64>(bits));
    return std::format("{}", bits);
}

}  // namespace

std::string dump(const Program& program) {
    std::string out;
    const auto reg = [](Reg r) { return std::format("%{}", r.id); };
    for (usize f = 0; f < program.functions.size(); ++f) {
        const auto& fun = program.functions[f];
        if (f) out += '\n';
        out += std::format("fun @{}(", fun.name);
        for (u32 p = 0; p < fun.params; ++p)
            out += std::format("{}%{}: {}", p ? ", " : "", p, type_name(fun.regs[p]));
        out += std::format(") -> {}{} {{\n", type_name(fun.result), f == program.main ? " entry" : "");
        for (usize b = 0; b < fun.blocks.size(); ++b) {
            const auto& block = fun.blocks[b];
            out += std::format("bb{}:\n", b);
            for (const auto& inst : block.insts) {
                out += "    ";
                if (inst.dst.valid()) out += std::format("{}: {} = ", reg(inst.dst), type_name(fun.regs[inst.dst.id]));
                out += op_name(inst.op);
                switch (inst.op) {
                    case Op::Const: out += ' ' + constant(fun.regs[inst.dst.id], inst.imm); break;
                    case Op::Call:
                        out += std::format(" @{}(", program.functions[inst.callee].name);
                        for (usize a = 0; a < inst.args.size(); ++a) out += (a ? ", " : "") + reg(inst.args[a]);
                        out += ')';
                        break;
                    default:
                        out += ' ' + reg(inst.a);
                        if (inst.b.valid()) out += ", " + reg(inst.b);
                        break;
                }
                out += '\n';
            }
            switch (block.term) {
                case Term::Br: out += std::format("    br bb{}\n", block.target); break;
                case Term::CondBr:
                    out += std::format("    condbr {}, bb{}, bb{}\n", reg(block.value), block.target, block.other);
                    break;
                case Term::Ret: out += block.value.valid() ? std::format("    ret {}\n", reg(block.value)) : "    ret\n"; break;
                case Term::Unreachable: out += "    unreachable\n"; break;
            }
        }
        out += "}\n";
    }
    return out;
}

}  // namespace phs::mir
