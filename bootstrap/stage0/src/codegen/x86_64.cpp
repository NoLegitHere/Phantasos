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

#include "codegen/x86_64.hpp"

#include <format>
#include <limits>
#include <vector>

namespace phs::x86_64 {
namespace {
using mir::Op;
using mir::Term;
using mir::Type;

enum Reg : u8 { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9 };
constexpr std::string_view kRegNames[] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9"};
constexpr std::string_view kDword[] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "r8d", "r9d"};
constexpr std::string_view kWord[] = {"ax", "cx", "dx", "bx", "sp", "bp", "si", "di", "r8w", "r9w"};
constexpr std::string_view kByte[] = {"al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil", "r8b", "r9b"};
constexpr Reg kArgs[] = {RDI, RSI, RDX, RCX, R8, R9};

// Condition codes, as the low nibble of jcc and setcc opcodes.
enum Cond : u8 { O = 0x0, B = 0x2, AE = 0x3, E = 0x4, NE = 0x5, BE = 0x6, A = 0x7, L = 0xC, GE = 0xD, LE = 0xE, G = 0xF };
std::string_view cond_name(Cond cc) {
    switch (cc) {
        case O: return "o";
        case B: return "b";
        case AE: return "ae";
        case E: return "e";
        case NE: return "ne";
        case BE: return "be";
        case A: return "a";
        case L: return "l";
        case GE: return "ge";
        case LE: return "le";
        case G: return "g";
    }
    unreachable();
}

std::string disp(i32 offset) { return offset < 0 ? std::format("[rbp-{}]", -offset) : std::format("[rbp+{}]", offset); }

// Encodes one function at a time into a shared .text buffer. Jumps use rel32
// fields patched once their labels are placed.
class Assembler {
public:
    Assembler(obj::Object& object, std::string* listing) : object_(object), listing_(listing) {}

    using Label = u32;
    Label label() {
        labels_.push_back(kUnplaced);
        return static_cast<Label>(labels_.size() - 1);
    }
    void place(Label l) { labels_[l] = here(); }
    void reset() {
        labels_.clear();
        fixups_.clear();
    }
    void patch() {
        for (const auto& [field, target] : fixups_) {
            PHS_ASSERT(labels_[target] != kUnplaced);
            put32(field, static_cast<u32>(static_cast<i64>(labels_[target]) - static_cast<i64>(field + 4)));
        }
    }
    u64 here() const { return object_.text.size(); }
    void note(std::string text) {
        if (listing_) lines_.push_back({here(), 0, std::move(text), true});
    }
    std::string render() const {
        std::string out;
        for (usize i = 0; i < lines_.size(); ++i) {
            const auto& line = lines_[i];
            if (line.header) {
                out += line.text + '\n';
                continue;
            }
            std::string bytes;
            for (u64 b = 0; b < line.size; ++b) bytes += std::format("{:02x}", object_.text[line.offset + b]);
            out += std::format("    {:04x}  {:<22} {}\n", line.offset, bytes, line.text);
        }
        return out;
    }

    // ------------------------------------------------------------ moves

    void load(Reg dst, i32 offset) { mem(0x8B, dst, offset, std::format("mov {}, qword {}", name(dst), disp(offset))); }
    void store(i32 offset, Reg src) { mem(0x89, src, offset, std::format("mov qword {}, {}", disp(offset), name(src))); }
    void mov_imm(Reg dst, u64 value) {
        const auto start = begin();
        const auto signed_value = static_cast<i64>(value);
        if (signed_value >= std::numeric_limits<i32>::min() && signed_value <= std::numeric_limits<i32>::max()) {
            rex(true, 0, dst);
            byte(0xC7);
            byte(modrm(0, dst));
            put(static_cast<u32>(value));
        } else {
            rex(true, 0, dst);
            byte(static_cast<u8>(0xB8 + (dst & 7)));
            for (int i = 0; i < 8; ++i) byte(static_cast<u8>(value >> (8 * i)));
        }
        end(start, std::format("mov {}, {}", name(dst), signed_value));
    }
    void mov(Reg dst, Reg src) { rr(0x89, src, dst, std::format("mov {}, {}", name(dst), name(src))); }

    // ------------------------------------------------------------ arithmetic

    // op r/m64, r64 forms: add 01, or 09, and 21, sub 29, xor 31, cmp 39.
    void alu(u8 opcode, std::string_view mnemonic, Reg dst, Reg src) {
        rr(opcode, src, dst, std::format("{} {}, {}", mnemonic, name(dst), name(src)));
    }
    void imul(Reg dst, Reg src) {
        const auto start = begin();
        rex(true, dst, src);
        byte(0x0F);
        byte(0xAF);
        byte(modrm(dst, src));
        end(start, std::format("imul {}, {}", name(dst), name(src)));
    }
    // F7 group: not /2, neg /3, mul /4, div /6, idiv /7.
    void group3(u8 ext, std::string_view mnemonic, Reg r) {
        const auto start = begin();
        rex(true, 0, r);
        byte(0xF7);
        byte(modrm(ext, r));
        end(start, std::format("{} {}", mnemonic, name(r)));
    }
    void cqo() { raw({0x48, 0x99}, "cqo"); }
    void zero_edx() { raw({0x31, 0xD2}, "xor edx, edx"); }
    void flip_bool() { raw({0x83, 0xF0, 0x01}, "xor eax, 1"); }
    void test(Reg r) { rr(0x85, r, r, std::format("test {}, {}", name(r), name(r))); }
    void shr(Reg r, u8 count) {
        const auto start = begin();
        rex(true, 0, r);
        byte(0xC1);
        byte(modrm(5, r));
        byte(count);
        end(start, std::format("shr {}, {}", name(r), count));
    }
    void setcc(Cond cc) {
        raw({0x0F, static_cast<u8>(0x90 + cc), 0xC0}, std::format("set{} al", cond_name(cc)));
        raw({0x0F, 0xB6, 0xC0}, "movzx eax, al");
    }
    // Re-establishes the 64-bit form of a narrow value in r.
    void normalize(Reg r, Type type) {
        const auto start = begin();
        std::string text;
        switch (type) {
            case Type::I8: rex(true, r, r); byte(0x0F); byte(0xBE); text = std::format("movsx {}, {}", name(r), kByte[r]); break;
            case Type::I16: rex(true, r, r); byte(0x0F); byte(0xBF); text = std::format("movsx {}, {}", name(r), kWord[r]); break;
            case Type::I32: rex(true, r, r); byte(0x63); text = std::format("movsxd {}, {}", name(r), kDword[r]); break;
            case Type::U8: rex(false, r, r); byte(0x0F); byte(0xB6); text = std::format("movzx {}, {}", kDword[r], kByte[r]); break;
            case Type::U16: rex(false, r, r); byte(0x0F); byte(0xB7); text = std::format("movzx {}, {}", kDword[r], kWord[r]); break;
            case Type::U32: rex(false, r, r); byte(0x89); text = std::format("mov {}, {}", kDword[r], kDword[r]); break;
            default: return;
        }
        byte(modrm(r, r));
        end(start, std::move(text));
    }

    // ------------------------------------------------------------ control

    void jump(Label target) { branch({0xE9}, target, "jmp"); }
    void jcc(Cond cc, Label target) { branch({0x0F, static_cast<u8>(0x80 + cc)}, target, std::format("j{}", cond_name(cc))); }
    void call(u32 symbol) {
        const auto start = begin();
        byte(0xE8);
        object_.relocations.push_back({here(), symbol, -4});
        put(0u);
        end(start, std::format("call {}", object_.symbols[symbol].name));
    }
    void push(Reg r) {
        const auto start = begin();
        if (r >= R8) byte(0x41);
        byte(static_cast<u8>(0x50 + (r & 7)));
        end(start, std::format("push {}", name(r)));
    }
    void adjust_rsp(i32 amount) {
        const auto start = begin();
        byte(0x48);
        byte(0x81);
        byte(amount < 0 ? 0xEC : 0xC4);
        put(static_cast<u32>(amount < 0 ? -amount : amount));
        end(start, std::format("{} rsp, {}", amount < 0 ? "sub" : "add", amount < 0 ? -amount : amount));
    }
    void mov_edi(u32 value) {
        const auto start = begin();
        byte(0xBF);
        put(value);
        end(start, std::format("mov edi, {}", value));
    }
    void prologue() {
        raw({0x55}, "push rbp");
        raw({0x48, 0x89, 0xE5}, "mov rbp, rsp");
    }
    void epilogue() {
        raw({0xC9}, "leave");
        raw({0xC3}, "ret");
    }
    void ud2() { raw({0x0F, 0x0B}, "ud2"); }

private:
    static constexpr u64 kUnplaced = std::numeric_limits<u64>::max();
    struct Line {
        u64 offset, size;
        std::string text;
        bool header = false;
    };

    static std::string_view name(Reg r) { return kRegNames[r]; }
    static u8 modrm(u8 reg, u8 rm) { return static_cast<u8>(0xC0 | (reg & 7) << 3 | (rm & 7)); }
    void byte(u8 b) { object_.text.push_back(b); }
    void put(u32 v) {
        for (int i = 0; i < 4; ++i) byte(static_cast<u8>(v >> (8 * i)));
    }
    void put32(u64 at, u32 v) {
        for (int i = 0; i < 4; ++i) object_.text[at + i] = static_cast<u8>(v >> (8 * i));
    }
    void rex(bool wide, u8 reg, u8 rm) {
        const u8 prefix = static_cast<u8>(0x40 | (wide ? 8 : 0) | (reg >= 8 ? 4 : 0) | (rm >= 8 ? 1 : 0));
        if (prefix != 0x40) byte(prefix);
    }
    u64 begin() const { return here(); }
    void end(u64 start, std::string text) {
        if (listing_) lines_.push_back({start, here() - start, std::move(text)});
    }
    void raw(std::initializer_list<u8> bytes, std::string text) {
        const auto start = begin();
        for (const u8 b : bytes) byte(b);
        end(start, std::move(text));
    }
    void rr(u8 opcode, Reg reg, Reg rm, std::string text) {
        const auto start = begin();
        rex(true, reg, rm);
        byte(opcode);
        byte(modrm(reg, rm));
        end(start, std::move(text));
    }
    // [rbp + disp32] operand.
    void mem(u8 opcode, Reg reg, i32 offset, std::string text) {
        const auto start = begin();
        rex(true, reg, RBP);
        byte(opcode);
        byte(static_cast<u8>(0x80 | (reg & 7) << 3 | RBP));
        put(static_cast<u32>(offset));
        end(start, std::move(text));
    }
    void branch(std::initializer_list<u8> opcode, Label target, std::string mnemonic) {
        const auto start = begin();
        for (const u8 b : opcode) byte(b);
        fixups_.push_back({here(), target});
        put(0u);
        end(start, std::format("{} .L{}", mnemonic, target));
    }

    obj::Object& object_;
    std::string* listing_;
    std::vector<u64> labels_;
    std::vector<std::pair<u64, Label>> fixups_;
    std::vector<Line> lines_;
};

class Emitter {
public:
    Emitter(const mir::Program& program, obj::Object& object, std::string* listing)
        : program_(program), object_(object), as_(object, listing) {}

    void run() {
        for (const auto& fun : program_.functions) object_.symbols.push_back({symbol_name(fun), true});
        trap_ = static_cast<u32>(object_.symbols.size());
        object_.symbols.push_back({std::string(kTrapSymbol), false});
        for (usize i = 0; i < program_.functions.size(); ++i) function(static_cast<u32>(i));
    }

    std::string listing() const { return as_.render(); }

private:
    static i32 slot(mir::Reg r) { return -8 * static_cast<i32>(r.id + 1); }

    void function(u32 index) {
        const auto& fun = program_.functions[index];
        auto& symbol = object_.symbols[index];
        // Keep functions 16-byte aligned; int3 fills the gap.
        while (object_.text.size() % 16) object_.text.push_back(0xCC);
        symbol.offset = as_.here();
        as_.note(std::format("{}:  ; {}", symbol.name, fun.name));
        as_.reset();
        blocks_.clear();
        for (usize b = 0; b < fun.blocks.size(); ++b) blocks_.push_back(as_.label());
        overflow_ = as_.label();
        divide_ = as_.label();
        used_overflow_ = used_divide_ = false;
        fun_ = &fun;

        as_.prologue();
        const auto frame = static_cast<i32>((8 * fun.regs.size() + 15) / 16 * 16);
        if (frame) as_.adjust_rsp(-frame);
        for (u32 p = 0; p < fun.params; ++p) {
            if (p < 6) as_.store(slot({p}), kArgs[p]);
            else {
                // Stack arguments sit above the return address and saved rbp.
                as_.load(RAX, 16 + 8 * static_cast<i32>(p - 6));
                as_.store(slot({p}), RAX);
            }
        }
        for (usize b = 0; b < fun.blocks.size(); ++b) {
            as_.place(blocks_[b]);
            as_.note(std::format("  bb{}:  ; .L{}", b, blocks_[b]));
            const auto& block = fun.blocks[b];
            for (const auto& inst : block.insts) instruction(inst);
            terminator(block, static_cast<u32>(b));
        }
        trap_path(overflow_, used_overflow_, Trap::Overflow, "overflow");
        trap_path(divide_, used_divide_, Trap::DivideByZero, "division by zero");
        as_.patch();
        symbol.size = as_.here() - symbol.offset;
    }

    void trap_path(Assembler::Label label, bool used, Trap code, std::string_view what) {
        if (!used) return;
        as_.place(label);
        as_.note(std::format("  .L{}:  ; trap: {}", label, what));
        as_.mov_edi(static_cast<u32>(code));
        as_.call(trap_);
        as_.ud2();
    }

    void overflow(Cond cc) {
        used_overflow_ = true;
        as_.jcc(cc, overflow_);
    }

    // Traps unless rax already holds a normalized value of type.
    void range_check(Type type) {
        const u32 width = mir::bit_width(type);
        if (width == 64) return;
        as_.mov(RDX, RAX);
        if (mir::is_signed(type)) {
            as_.normalize(RDX, type);
            as_.alu(0x39, "cmp", RDX, RAX);
        } else as_.shr(RDX, static_cast<u8>(width));
        overflow(NE);
    }

    void instruction(const mir::Inst& inst) {
        if (inst.op == Op::Call) return call(inst);
        const Type type = fun_->regs[inst.dst.id];
        const Type operand = inst.a.valid() ? fun_->regs[inst.a.id] : type;
        const bool sign = mir::is_signed(operand);
        const bool wide = mir::bit_width(type) == 64;
        if (inst.a.valid()) as_.load(RAX, slot(inst.a));
        if (inst.b.valid()) as_.load(RCX, slot(inst.b));
        switch (inst.op) {
            case Op::Const: as_.mov_imm(RAX, inst.imm); break;
            case Op::Copy: case Op::Widen: break;
            case Op::Truncate: as_.normalize(RAX, type); break;
            case Op::Neg:
                as_.group3(3, "neg", RAX);
                if (wide) overflow(O);
                else range_check(type);
                break;
            case Op::Not: as_.flip_bool(); break;
            case Op::BitNot:
                as_.group3(2, "not", RAX);
                as_.normalize(RAX, type);
                break;
            case Op::Add: case Op::Sub:
                as_.alu(inst.op == Op::Add ? 0x01 : 0x29, inst.op == Op::Add ? "add" : "sub", RAX, RCX);
                if (wide) overflow(sign ? O : B);
                else range_check(type);
                break;
            case Op::Mul:
                if (wide && !sign) {
                    as_.group3(4, "mul", RCX);  // rdx:rax; OF is set when rdx is not zero
                    overflow(O);
                } else {
                    as_.imul(RAX, RCX);
                    if (wide) overflow(O);
                    else range_check(type);
                }
                break;
            case Op::Div: case Op::Rem: divide(inst.op, type, sign); break;
            case Op::WrapAdd: as_.alu(0x01, "add", RAX, RCX); as_.normalize(RAX, type); break;
            case Op::WrapSub: as_.alu(0x29, "sub", RAX, RCX); as_.normalize(RAX, type); break;
            case Op::WrapMul: as_.imul(RAX, RCX); as_.normalize(RAX, type); break;
            case Op::And: as_.alu(0x21, "and", RAX, RCX); break;
            case Op::Or: as_.alu(0x09, "or", RAX, RCX); break;
            case Op::Xor: as_.alu(0x31, "xor", RAX, RCX); break;
            case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: {
                as_.alu(0x39, "cmp", RAX, RCX);
                as_.setcc(compare(inst.op, sign));
                break;
            }
            case Op::Call: break;
        }
        as_.store(slot(inst.dst), RAX);
    }

    static Cond compare(Op op, bool sign) {
        switch (op) {
            case Op::Eq: return E;
            case Op::Ne: return NE;
            case Op::Lt: return sign ? L : B;
            case Op::Le: return sign ? LE : BE;
            case Op::Gt: return sign ? G : A;
            case Op::Ge: return sign ? GE : AE;
            default: unreachable();
        }
    }

    // rax / rcx. A zero divisor traps, and so does the signed minimum divided
    // by -1, for both quotient and remainder.
    void divide(Op op, Type type, bool sign) {
        as_.test(RCX);
        used_divide_ = true;
        as_.jcc(E, divide_);
        if (sign) {
            const auto ok = as_.label();
            const u32 width = mir::bit_width(type);
            as_.mov_imm(RDX, ~u64{0});
            as_.alu(0x39, "cmp", RCX, RDX);
            as_.jcc(NE, ok);
            as_.mov_imm(RDX, ~u64{0} << (width - 1));
            as_.alu(0x39, "cmp", RAX, RDX);
            overflow(E);
            as_.place(ok);
            as_.cqo();
            as_.group3(7, "idiv", RCX);
        } else {
            as_.zero_edx();
            as_.group3(6, "div", RCX);
        }
        if (op == Op::Rem) as_.mov(RAX, RDX);
    }

    void call(const mir::Inst& inst) {
        const auto count = inst.args.size();
        const auto stacked = count > 6 ? count - 6 : 0;
        // rsp stays 16-byte aligned at the call.
        const i32 pad = stacked % 2 ? 8 : 0;
        if (pad) as_.adjust_rsp(-pad);
        for (usize i = count; i-- > 6;) {
            as_.load(RAX, slot(inst.args[i]));
            as_.push(RAX);
        }
        for (usize i = 0; i < count && i < 6; ++i) as_.load(kArgs[i], slot(inst.args[i]));
        as_.call(inst.callee);
        if (const auto cleanup = static_cast<i32>(8 * stacked) + pad) as_.adjust_rsp(cleanup);
        if (inst.dst.valid()) as_.store(slot(inst.dst), RAX);
    }

    void terminator(const mir::Block& block, u32 index) {
        switch (block.term) {
            case Term::Br:
                if (block.target != index + 1) as_.jump(blocks_[block.target]);
                break;
            case Term::CondBr:
                as_.load(RAX, slot(block.value));
                as_.test(RAX);
                if (block.other == index + 1) as_.jcc(NE, blocks_[block.target]);
                else if (block.target == index + 1) as_.jcc(E, blocks_[block.other]);
                else {
                    as_.jcc(NE, blocks_[block.target]);
                    as_.jump(blocks_[block.other]);
                }
                break;
            case Term::Ret:
                if (block.value.valid()) as_.load(RAX, slot(block.value));
                as_.epilogue();
                break;
            case Term::Unreachable: as_.ud2(); break;
        }
    }

    const mir::Program& program_;
    obj::Object& object_;
    Assembler as_;
    const mir::Function* fun_ = nullptr;
    std::vector<Assembler::Label> blocks_;
    Assembler::Label overflow_ = 0, divide_ = 0;
    bool used_overflow_ = false, used_divide_ = false;
    u32 trap_ = 0;
};

}  // namespace

std::string symbol_name(const mir::Function& function) {
    return std::format("_P{}{}", function.name.size(), function.name);
}

obj::Object emit(const mir::Program& program, std::string* listing) {
    obj::Object object;
    Emitter emitter(program, object, listing);
    emitter.run();
    if (listing) *listing = emitter.listing();
    return object;
}

}  // namespace phs::x86_64
