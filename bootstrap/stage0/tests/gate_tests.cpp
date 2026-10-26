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

#include <algorithm>
#include <initializer_list>
#include <string>
#include <vector>

#include "frontend/session.hpp"
#include "syntax/ast_dump.hpp"

using namespace phs;

namespace {
struct Frontend {
    FrontendSession session;

    bool gate(std::string_view source, std::initializer_list<DiagCode> expected = {}, bool dump = true) {
        const auto id = session.sources().add("gate.phs", source);
        CHECK(id);
        const auto* syntax = session.parse(*id);
        if (session.diagnostics().has_errors(*id)) session.diagnostics().render(stderr);
        CHECK(!session.diagnostics().has_errors(*id));
        const auto before = dump ? dump_ast(*syntax, session.names()) : std::string{};
        const bool ok = session.check_codegen(*id);
        if (dump) CHECK(dump_ast(*syntax, session.names()) == before);
        std::vector<DiagCode> actual, wanted(expected);
        for (const auto& d : session.diagnostics().all()) actual.push_back(d.code);
        std::ranges::sort(actual);
        std::ranges::sort(wanted);
        if (actual != wanted) session.diagnostics().render(stderr);
        CHECK(actual == wanted);
        CHECK(ok == wanted.empty());
        // Results are cached per file, and diagnostics are not repeated.
        const auto errors = session.diagnostics().error_count();
        CHECK(session.check_codegen(*id) == ok);
        CHECK(session.diagnostics().error_count() == errors);
        return ok;
    }
};

void accepted_subset() {
    Frontend f;
    f.gate(R"(
fun fib(n: Int): Int {
    if n < 2 { return n }
    return fib(n - 1) + fib(n - 2)
}
fun narrow(x: Int64): Int32 {
    var y: Int32 = 0
    while y < 3 && !(x == 0) {
        y += 1
        if y == 2 { continue } else { y = y &+ 0 }
    }
    loop { break }
    return y
}
fun pick(c: Bool, a: UInt16, b: UInt16): UInt16 = if c { a & b } else { a | ~b }
fun wide(x: Int32): Int64 = Int64(x) * 2
fun stop(): Never {
    loop {}
}
fun main(): Int {
    let n: Int = if fib(3) > 1 { 1 } else if true { 2 } else { stop() }
    return n + Int(wide(narrow(4)))
}
)");
    Frontend unit;
    unit.gate("fun main() {}\n");
    Frontend never;
    never.gate("fun main(): Never {\n    loop {}\n}\n");
}

void rejected_items() {
    Frontend f;
    f.gate(R"(
struct S {}
enum E { A, B }
let g = 1
type T = Int
fun real(x: Float32): Float32 = x
fun opt(x: Int?) {}
fun gen<T>(x: T) {}
fun bad() throws {}
fun conv(x: inout Int) {}
fun dflt(x: Int = 0) {}
async fun later() {}
fun main() {}
)", {DiagCode::UnsupportedCodegen, DiagCode::UnsupportedCodegen, DiagCode::UnsupportedCodegen,
     DiagCode::UnsupportedCodegen, DiagCode::UnsupportedCodegen, DiagCode::UnsupportedCodegen,
     DiagCode::UnsupportedCodegen, DiagCode::UnsupportedCodegen, DiagCode::UnsupportedCodegen,
     DiagCode::UnsupportedCodegen, DiagCode::UnsupportedCodegen, DiagCode::UnsupportedCodegen});
}

void rejected_bodies() {
    Frontend f;
    f.gate(R"(
fun other(): Int = 1
fun main(): Int {
    let c = 'x'
    let r = 1 << 2
    for i in 0..3 {}
    defer {}
    let m: Int = match 1 { _ -> 2 }
    let u: Int = unsafe { 3 }
    let h = other
    let v = if true { 1 } else { 2 }
    return v + v
}
)", {DiagCode::UnsupportedCodegen, DiagCode::UnsupportedCodegen, DiagCode::UnsupportedCodegen,
     DiagCode::UnsupportedCodegen, DiagCode::UnsupportedCodegen, DiagCode::UnsupportedCodegen,
     DiagCode::UnsupportedCodegen, DiagCode::UnsupportedCodegen});
}

void entry_points() {
    Frontend none;
    none.gate("fun helper() {}\n", {DiagCode::InvalidEntryPoint});
    Frontend params;
    params.gate("fun main(x: Int) {}\n", {DiagCode::InvalidEntryPoint});
    Frontend result;
    result.gate("fun main(): Bool = true\n", {DiagCode::InvalidEntryPoint});
    Frontend overloaded;
    overloaded.gate("fun main() {}\nfun main(x: Int) {}\n", {DiagCode::InvalidEntryPoint});
    // A rejected result type is reported once, as unsupported.
    Frontend real;
    real.gate("fun main(): Float64 = 1.0\n", {DiagCode::UnsupportedCodegen});
}

void gated_by_check_errors() {
    FrontendSession session;
    const auto id = session.sources().add("gate.phs", "fun main(): Int {\n    return true\n}\nlet s = \"x\"\n");
    CHECK(id);
    CHECK(!session.check_codegen(*id));
    CHECK(session.diagnostics().error_count() == 1);
    CHECK(session.diagnostics().all().front().code == DiagCode::ScalarTypeMismatch);
}

void long_trees() {
    std::string chain = "fun main(): Int {\n    let c: Bool = true";
    for (int i = 0; i < 50000; ++i) chain += " || false";
    chain += "\n    if c { return 1 }\n    return 0\n}\n";
    Frontend f;
    f.gate(chain, {}, false);
    std::string nested = "fun main() {\n    var i = 0\n";
    for (int i = 0; i < 40; ++i) nested += "while i < 1 {\n";
    nested += "i += 1\n";
    for (int i = 0; i < 40; ++i) nested += "}\n";
    nested += "}\n";
    Frontend g;
    g.gate(nested);
}
}  // namespace

int main() {
    accepted_subset();
    rejected_items();
    rejected_bodies();
    entry_points();
    gated_by_check_errors();
    long_trees();
    std::println("gate tests passed");
}
