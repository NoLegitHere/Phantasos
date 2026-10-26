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

#include <limits>
#include <string>
#include <vector>

#include "codegen/mir.hpp"
#include "frontend/session.hpp"

using namespace phs;
using mir::Op;
using mir::Term;
using mir::Type;

namespace {

// A reference interpreter for MIR semantics: checked operations trap.
struct Outcome {
    bool trapped = false;
    u64 value = 0;
};

u64 normalize(Type type, u64 bits) {
    const u32 width = mir::bit_width(type);
    if (width == 1) return bits & 1;
    if (width == 0 || width == 64) return bits;
    const u64 mask = (u64{1} << width) - 1;
    bits &= mask;
    if (mir::is_signed(type) && (bits >> (width - 1)) & 1) bits |= ~mask;
    return bits;
}

bool fits(Type type, u64 bits) { return normalize(type, bits) == bits; }

class Interpreter {
public:
    explicit Interpreter(const mir::Program& program) : program_(program) {}

    Outcome run() { return call(program_.main, {}); }

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
                const Type type = fun.regs[inst.dst.id];
                const Type operand = inst.a.valid() ? fun.regs[inst.a.id] : type;
                const u64 a = inst.a.valid() ? regs[inst.a.id] : 0, b2 = inst.b.valid() ? regs[inst.b.id] : 0;
                const bool sign = mir::is_signed(operand);
                const auto sa = static_cast<i64>(a), sb = static_cast<i64>(b2);
                u64 out = 0;
                bool trap = false;
                switch (inst.op) {
                    case Op::Const: out = normalize(type, inst.imm); CHECK(out == inst.imm); break;
                    case Op::Copy: out = a; break;
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
                        trap = b2 == 0 || (sign && sb == -1 && sa == min);
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
                    case Op::Call: break;
                }
                if (trap) return {true, 0};
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
};

struct Frontend {
    FrontendSession session;
    const mir::Program* program = nullptr;

    const mir::Program& lower(std::string_view source) {
        const auto id = session.sources().add("mir.phs", source);
        CHECK(id);
        program = session.lower(*id);
        if (!program) session.diagnostics().render(stderr);
        CHECK(program);
        CHECK(session.lower(*id) == program);
        // Every block is reachable and every jump targets a block.
        for (const auto& fun : program->functions)
            for (const auto& b : fun.blocks) {
                if (b.term == Term::Br || b.term == Term::CondBr) CHECK(b.target < fun.blocks.size());
                if (b.term == Term::CondBr) CHECK(b.other < fun.blocks.size());
            }
        return *program;
    }
    Outcome run(std::string_view source) { return Interpreter(lower(source)).run(); }
    std::string dump(std::string_view source) { return mir::dump(lower(source)); }
};

i64 result(std::string_view source) {
    Frontend f;
    const auto outcome = f.run(source);
    CHECK(!outcome.trapped);
    return static_cast<i64>(outcome.value);
}

bool traps(std::string_view source) {
    Frontend f;
    return f.run(source).trapped;
}

void target_program() {
    CHECK(result(R"(
fun fib(n: Int): Int {
    if n < 2 { return n }
    return fib(n - 1) + fib(n - 2)
}
fun sumTo(limit: Int): Int {
    var total = 0
    var i = 1
    while i <= limit {
        if i % 3 == 0 || i % 5 == 0 { total += i }
        i += 1
    }
    return total
}
fun main(): Int {
    return fib(10) - sumTo(9) + 10
}
)") == 42);
}

void control_flow() {
    CHECK(result(R"(
fun classify(x: Int): Int {
    if x < 0 { return -1 } else if x == 0 { return 0 } else if x < 10 { return 1 }
    return 2
}
fun loops(n: Int): Int {
    var total = 0
    var i = 0
    loop {
        i += 1
        if i > n { break }
        if i % 2 == 0 { continue }
        var j = 0
        while true {
            j += 1
            if j >= i { break }
            total += 1
        }
    }
    return total
}
fun pick(c: Bool): Int = if c { 7 } else { 9 }
fun main(): Int {
    let x: Int
    if pick(true) == 7 { x = 100 } else { x = 200 }
    let y: Int = if x > 150 { 1 } else if x > 50 { 2 } else { 3 }
    let z = x + y
    return z + classify(-5) + classify(0) * 10 + classify(5) + classify(50) + loops(5) + pick(false)
}
)") == 102 - 1 + 0 + 1 + 2 + 6 + 9);
}

void arithmetic() {
    CHECK(result(R"(
fun main(): Int {
    let a: Int8 = 127
    let b: Int8 = a &+ 1
    let c: UInt8 = 200
    let d: UInt8 = c &* 2
    let e: UInt16 = ~UInt16(c)
    let m: Int64 = -9223372036854775808
    let t: Int8 = Int8.truncating(300)
    var bits: UInt32 = 12
    bits &= 10
    bits |= 1
    bits ^= 3
    let n: Int = -7 / 2 + -7 % 2
    let flag: Int = if m == -9223372036854775807 - 1 { 1000 } else { 0 }
    return Int(b) + Int(d) + Int(e) + flag + Int(t) + Int(bits) + n
}
)") == -128 + 144 + 65335 + 1000 + 44 + 10 - 3 - 1);
    CHECK(traps("fun main(): Int {\n    var x: Int8 = 127\n    x += 1\n    return 0\n}\n"));
    CHECK(traps("fun main(): Int {\n    let x: UInt = 0\n    let y = x - 1\n    return 0\n}\n"));
    CHECK(traps("fun zero(): Int = 0\nfun main(): Int = 5 / zero()\n"));
    CHECK(traps("fun zero(): Int = 0\nfun main(): Int = 5 % zero()\n"));
    CHECK(traps("fun minus(): Int = -1\nfun main(): Int = (-9223372036854775807 - 1) / minus()\n"));
    CHECK(traps("fun main(): Int {\n    let x: Int32 = -2147483647 - 1\n    return Int(-x)\n}\n"));
    CHECK(traps("fun main(): Int {\n    let x: Int16 = 300\n    return Int(x * x)\n}\n"));
}

void short_circuit() {
    // The right operand would trap if it ran.
    CHECK(result(R"(
fun boom(): Bool = 1 / 0 == 1
fun main(): Int {
    let a = false && boom()
    let b = true || boom()
    if a || !b { return 1 }
    return 0
}
)") == 0);
    CHECK(traps("fun boom(): Bool = 1 / 0 == 1\nfun main(): Int {\n    let a = true && boom()\n    return 0\n}\n"));
}

void calls_and_order() {
    Frontend f;
    const auto text = f.dump(R"(
fun sub(a: Int, b: Int): Int = a - b
fun sub(a: Int8): Int8 = a
fun stop(): Never {
    loop {}
}
fun noop() {}
fun main(): Int {
    noop()
    let x: Int = if true { sub(b: 1, a: 5) } else { stop() }
    return x
}
)");
    CHECK(text.contains("fun @sub.1(%0: i8) -> i8"));
    CHECK(text.contains("fun @stop() -> never"));
    CHECK(text.contains("fun @main() -> i64 entry"));
    // Labeled arguments run in source order and pass in parameter order.
    CHECK(text.contains("    %2: i64 = const 1\n    %3: i64 = const 5\n    %4: i64 = call @sub(%3, %2)\n"));
    // A call to a Never function ends its block.
    CHECK(text.contains("    call @stop()\n    unreachable\n"));
    CHECK(text.contains("    call @noop()\n"));
    Frontend g;
    CHECK(g.run("fun sub(a: Int, b: Int): Int = a - b\nfun main(): Int = sub(b: 1, a: 5)\n").value == 4);
    Frontend unit;
    CHECK(unit.run("fun main() {\n    let same = () == ()\n    if !same { return }\n}\n").value == 0);
}

void rejected_files() {
    FrontendSession session;
    const auto id = session.sources().add("mir.phs", "fun main(): Float64 = 1.0\n");
    CHECK(id);
    CHECK(!session.lower(*id));
    CHECK(session.diagnostics().error_count() == 1);
    CHECK(!session.lower(*id));
    CHECK(session.diagnostics().error_count() == 1);
}

void long_trees() {
    std::string chain = "fun main(): Int {\n    let c: Bool = false";
    for (int i = 0; i < 50000; ++i) chain += " || false";
    chain += " || true\n    if c { return 1 }\n    return 0\n}\n";
    CHECK(result(chain) == 1);
    std::string sum = "fun main(): Int = 0";
    for (int i = 0; i < 50000; ++i) sum += " + 1";
    sum += "\n";
    CHECK(result(sum) == 50000);
    std::string nested = "fun main(): Int {\n    var i = 0\n";
    for (int i = 0; i < 40; ++i) nested += "while i < 1 {\n";
    nested += "i += 1\n";
    for (int i = 0; i < 40; ++i) nested += "}\n";
    nested += "return i\n}\n";
    CHECK(result(nested) == 1);
}
}  // namespace

int main() {
    target_program();
    control_flow();
    arithmetic();
    short_circuit();
    calls_and_order();
    rejected_files();
    long_trees();
    std::println("mir tests passed");
}
