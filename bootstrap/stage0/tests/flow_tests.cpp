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
using Type = ScalarType;

namespace {
struct Frontend {
    FrontendSession session;
    CheckResult result{};

    void check(std::string_view source, std::initializer_list<DiagCode> expected = {}, bool dump = true) {
        const auto id = session.sources().add("flow.phs", source);
        CHECK(id);
        const auto* syntax = session.parse(*id);
        if (session.diagnostics().has_errors(*id)) session.diagnostics().render(stderr);
        CHECK(!session.diagnostics().has_errors(*id));
        const auto before = dump ? dump_ast(*syntax, session.names()) : std::string{};
        result = session.check(*id);
        CHECK(result.scalar_types);
        if (dump) CHECK(dump_ast(*syntax, session.names()) == before);
        std::vector<DiagCode> actual, wanted(expected);
        for (const auto& d : session.diagnostics().all()) actual.push_back(d.code);
        std::ranges::sort(actual);
        std::ranges::sort(wanted);
        if (actual != wanted) session.diagnostics().render(stderr);
        CHECK(actual == wanted);
        const auto errors = session.diagnostics().error_count();
        CHECK(session.check(*id).scalar_types == result.scalar_types);
        CHECK(session.diagnostics().error_count() == errors);
    }

    const ast::VarItem& global(std::string_view name) const {
        for (const auto& b : result.resolution->bindings())
            if (b.kind == BindingKind::Variable && session.names().str(b.name.sym) == name) return *ast::cast<ast::VarItem>(b.item);
        CHECK(false);
        unreachable();
    }
    Type tail(const ast::Expr* block) const {
        const auto* stmts = ast::cast<ast::BlockExpr>(block);
        return result.scalar_types->expression(ast::cast<ast::ExprStmt>(stmts->stmts.back())->expr);
    }
};

void accepted_paths() {
    Frontend f;
    f.check(R"(
fun branches(x: Int): Int {
    if x > 0 { return 1 } else if x < 0 { return -1 } else { return 0 }
}
fun forever(): Int {
    loop {}
}
fun exits(x: Int): Int {
    loop {
        if x > 3 { return x }
    }
}
fun aborts(x: Int): Int {
    if x > 0 { return 1 }
    panic("negative")
}
fun deferred(x: Int): Int {
    let y: Int
    if x > 0 { y = 1 } else { y = 2 }
    return y
}
fun counted(limit: Int): Int {
    var total: Int
    total = 0
    var i = 0
    while i < limit {
        total += i
        i += 1
    }
    return total
}
fun matched(x: Int): Int {
    var y: Int
    match x {
        0 -> { y = 1 }
        _ -> { y = 2 }
    }
    return y
}
fun written(x: inout Int) { x = 3 }
fun unit(): Unit {}
fun expression(x: Int): Int = if x > 0 { 1 } else { 2 }
var counter = 0
fun bump() { counter += 1 }
fun main(): Int { return 0 }
)");
}

void missing_returns() {
    Frontend f;
    f.check(R"(
fun partial(a: Int): Int {
    if a > 0 { return 1 }
}
fun chain(a: Int): Int {
    if a > 1 { return 1 } else if a > 0 { return 2 }
}
fun loops(c: Bool): Int {
    while c { return 1 }
}
fun broken(): Int {
    loop { break }
}
fun empty(): Bool {}
)", {DiagCode::MissingReturn, DiagCode::MissingReturn, DiagCode::MissingReturn, DiagCode::MissingReturn,
     DiagCode::MissingReturn});
}

void initialization() {
    Frontend f;
    f.check(R"(
fun partial(c: Bool): Int {
    var y: Int
    if c { y = 1 }
    return y
}
fun looped(c: Bool): Int {
    var y: Int
    while c { y = 1 }
    return y
}
fun compound(): Int {
    var y: Int
    y += 1
    return 0
}
fun short(a: Bool): Int {
    var y: Int
    if a && (if a { y = 1
        true } else { false }) { return y }
    return 0
}
fun dead(): Int {
    var y: Int
    return 1
    return y
}
)", {DiagCode::UninitializedUse, DiagCode::UninitializedUse, DiagCode::UninitializedUse, DiagCode::UninitializedUse});
}

void immutability() {
    Frontend f;
    f.check(R"(
let limit = 1
fun parameter(x: Int) { x = 1 }
fun local() {
    let x = 1
    x = 2
}
fun twice() {
    let y: Int
    y = 1
    y = 2
}
fun repeated(c: Bool) {
    let y: Int
    while c { y = 1 }
}
fun global() { limit = 2 }
fun bound(values: [Int]) {
    for v in values { v = 1 }
}
)", {DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment,
     DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment});
}

void branch_results() {
    Frontend f;
    f.check(R"(
fun pick(d: Bool): Int = if d { 1 } else if !d { 2 } else { 3 }
fun maybe(d: Bool): Int? = if d { 5 }
let wide: Int64 = if true { 1 } else { 2 }
let some: Int? = if true { 1 } else { none }
)");
    CHECK(f.tail(ast::cast<ast::IfExpr>(f.global("wide").init)->then_block) == Type::Int64);
    CHECK(f.tail(ast::cast<ast::IfExpr>(f.global("wide").init)->else_branch) == Type::Int64);

    Frontend errors;
    errors.check(R"(
fun missing(d: Bool): Int = if d { 5 }
fun mismatch(a: Int): Int = if a > 0 { true } else { 2 }
let unit: Int = if true { 1 } else { }
let range: UInt8 = if true { 300 } else { 2 }
fun exits(): Int = unsafe { loop { break } }
fun elements(flag: Bool): [Int] = if flag { [if flag { true } else { 2 }] } else { [1] }
let narrow: [UInt8] = [if true { 300 } else { 1 }]
)", {DiagCode::IfWithoutElse, DiagCode::ScalarTypeMismatch, DiagCode::ScalarTypeMismatch, DiagCode::ScalarLiteralRange,
     DiagCode::ScalarTypeMismatch, DiagCode::ScalarTypeMismatch, DiagCode::ScalarLiteralRange});

    Frontend loops;
    loops.check(R"(
fun forever(): Int = unsafe { loop {} }
fun returns(): Int = unsafe { loop {
    return 1
    break
} }
fun inner(c: Bool): Int = unsafe {
    loop {
        while c { break }
        loop { break }
    }
}
)");
}

void gated_by_type_errors() {
    // Flow analysis runs only after scalar typing succeeds.
    Frontend f;
    f.check(R"(
fun f(): Int {
    let x: Int = true
}
)", {DiagCode::ScalarTypeMismatch});
}

void caught_errors_guards_and_conditions() {
    Frontend f;
    f.check(R"(
fun caught(): Int {
    let x = try? unsafe { throw 1 }
}
fun caughtBeforeInit(c: Bool): Int {
    var y: Int
    let z = try? unsafe {
        if c { throw 1 }
        y = 2
        1
    }
    return y
}
fun guarded(v: Int) {
    let x: Int
    match v {
        _ if unsafe { x = 1
            false } -> {}
        _ -> { x = 2 }
    }
}
fun condition(c: Bool) {
    let x: Int
    while unsafe { x = 1
        c } {}
}
fun defaulted(value: Int = unsafe { var x: Int
    x }) {}
let initializer = unsafe { var y: Int
    y }
fun maybe(): Never? = none
fun optionalNever(): Int {
    maybe()
}
fun cleanupThenWrite() {
    let x: Int
    defer { x = 1 }
    x = 2
}
fun twoCleanups() {
    let x: Int
    defer { x = 1 }
    defer { x = 2 }
}
fun rewritesParam(a: Int, b: Int = unsafe { a = 1
    0 }) {}
)", {DiagCode::MissingReturn, DiagCode::UninitializedUse, DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment,
     DiagCode::MissingReturn, DiagCode::ImmutableAssignment, DiagCode::ImmutableAssignment,
     DiagCode::UninitializedUse, DiagCode::UninitializedUse, DiagCode::ImmutableAssignment});

    Frontend valid;
    valid.check(R"(
fun arms(v: Int): Int {
    let x: Int
    match v {
        0 -> { x = 1 }
        _ -> { x = 2 }
    }
    return x
}
fun aborts(): Int {
    let x = try? panic("stop")
}
fun cleanupOnly() {
    let x: Int
    defer { x = 1 }
}
fun catchAll(v: Int): Int {
    var x: Int
    match v {
        _ -> { x = 1 }
        0 -> {}
    }
    return x
}
fun guardFirst(v: Int): Int {
    var x: Int
    match v {
        _ if unsafe { x = 1
            false } -> {}
        _ -> {}
    }
    return x
}
fun initialized(): Int {
    var y: Int
    let z = try? unsafe {
        y = 1
        throw 2
    }
    return y
}
)");
}

void long_trees() {
    std::string chain = "fun f(a: Bool): Int {\n    var y: Int\n    let c = a";
    for (int i = 0; i < 50000; ++i) chain += " || a";
    chain += "\n    if c { y = 1 } else { y = 2 }\n    return y\n}\n";
    Frontend f;
    f.check(chain, {}, false);

    std::string nested = "y = 1\n";
    for (int i = 0; i < 40; ++i) nested = "while a {\n" + nested + "}\n";
    Frontend loops;
    loops.check("fun g(a: Bool): Int {\n    let y: Int\n" + nested + "    return y\n}\n",
                {DiagCode::ImmutableAssignment, DiagCode::UninitializedUse});
}
}  // namespace

int main() {
    accepted_paths();
    missing_returns();
    initialization();
    immutability();
    branch_results();
    gated_by_type_errors();
    caught_errors_guards_and_conditions();
    long_trees();
}
