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
#include "mir_interpreter.hpp"
#include "frontend/session.hpp"

using namespace phs;
using mir::Op;
using mir::Term;
using mir::Type;

using test::Interpreter;
using test::Outcome;

namespace {

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

void writes_during_evaluation() {
    // An operand read before a later operand assigns the same local keeps its value.
    CHECK(result(R"(
fun sub(a: Int, b: Int): Int = a - b
fun main(): Int {
    var x = 1
    let y = sub(x, if true {
        x = 2
        3
    } else { 3 })
    let z = sub(x * 10, if true {
        x = 5
        1
    } else { 1 })
    return y * 100 + z + x
}
)") == -200 + 19 + 5);
    // Compound assignment reads its target before its value.
    CHECK(result(R"(
fun main(): Int {
    var x = 1
    x += if true {
        x = 10
        2
    } else { 2 }
    return x
}
)") == 3);
    // A diverging operand ends the block without a value.
    Frontend f;
    const auto text = f.dump("fun stop(): Never {\n    loop {}\n}\nfun main(): Int = -stop()\nfun flag(): Bool = !stop()\n");
    CHECK(text.contains("    call @stop()\n    unreachable\n"));
    CHECK(!text.contains("neg") && !text.contains("not"));
}

void structs() {
    Frontend f;
    const auto& program = f.lower(R"(
struct Flags(on: Bool, level: Int8, mask: UInt32)
struct Point(x: Int, y: Int)
struct Label(text: String, at: Point, tag: Flags)
struct Bits(a: Bool, b: Bool, c: Bool)
fun tagged(l: Label): Int = Int(l.tag.level) + l.at.y
fun main(): Int {
    let l = Label(tag: Flags(mask: 1, level: -2, on: true), text: "x", at: Point(y: 5, x: 3))
    let bits = Bits(a: false, b: true, c: false)
    if bits.b { return tagged(l) }
    return 0
}
)");
    // Layouts follow declaration order, nested structs first, with fields by
    // decreasing alignment (abi.md A.2).
    CHECK(program.layouts.size() == 4);
    const auto& flags = program.layouts[0];
    CHECK(flags.name == "Flags" && flags.offsets == std::vector<u32>{4, 5, 0} && flags.size == 8 && flags.align == 4);
    const auto& label = program.layouts[2];
    CHECK(label.name == "Label" && label.offsets == std::vector<u32>{0, 16, 32} && label.size == 40 && label.align == 8);
    CHECK(mir::size_of(program, label.fields[1]) == 16 && mir::words(program, mir::aggregate(2)) == 5);
    const auto& bits = program.layouts[3];
    CHECK(bits.offsets == std::vector<u32>{0, 1, 2} && bits.size == 3 && bits.align == 1);
    CHECK(Interpreter(program).run().value == 3);
    const auto text = mir::dump(program);
    CHECK(text.starts_with("struct @Flags { on: bool +4, level: i8 +5, mask: u32 +0 } size 8 align 4\n\n"));
    // Labeled arguments run in source order and fill fields in declaration order.
    CHECK(text.contains("    %0: u32 = const 1\n    %1: i8 = const -2\n    %2: bool = const true\n"
                        "    %3: @Flags = make %2, %1, %0\n"));
    CHECK(text.contains("= field %0, tag\n"));
    // A diverging field value ends the block before the struct is made.
    Frontend g;
    const auto stop = g.dump("struct P(x: Int, y: Int)\nfun stop(): Never {\n    loop {}\n}\n"
                             "fun main(): Int {\n    let p = P(x: 1, y: stop())\n    return p.x\n}\n");
    CHECK(stop.contains("    call @stop()\n    unreachable\n") && !stop.contains("make"));
    // Field values trap like any other arithmetic.
    CHECK(traps("struct P(x: Int8)\nfun main(): Int {\n    let p = P(x: 127)\n    return Int(P(x: p.x + 1).x)\n}\n"));
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
    writes_during_evaluation();
    structs();
    rejected_files();
    long_trees();
    std::println("mir tests passed");
}
