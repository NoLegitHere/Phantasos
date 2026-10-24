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

#include "sema/validate.hpp"
#include "syntax/ast_dump.hpp"
#include "syntax/lexer.hpp"
#include "syntax/parser.hpp"

using namespace phs;

namespace {
struct Frontend {
    SourceManager sources;
    Interner names;
    DiagEngine diag{sources};
    Arena arena;

    Frontend() { seed_keywords(names); }

    const ast::File* parse(std::string_view source) {
        const usize before = diag.error_count();
        const auto file = sources.add("test.phs", source);
        CHECK(file);
        const auto* ast = parse_file(*file, lex_file(sources, *file, names, diag), arena, names, diag);
        if (diag.error_count() != before) diag.render(stderr);
        CHECK(diag.error_count() == before);
        return ast;
    }

    void check(std::string_view source, std::initializer_list<DiagCode> expected) {
        const auto* file = parse(source);
        const std::string before = dump_ast(*file, names);
        validate_file(*file, names, diag);
        CHECK(dump_ast(*file, names) == before);
        if (diag.error_count() != expected.size()) diag.render(stderr);
        CHECK(diag.error_count() == expected.size());
        std::vector<DiagCode> actual;
        for (const Diagnostic& d : diag.all()) {
            CHECK(d.span.file == file->file);
            actual.push_back(d.code);
        }
        std::vector<DiagCode> wanted(expected);
        std::ranges::sort(actual);
        std::ranges::sort(wanted);
        CHECK(actual == wanted);
    }
};

void duplicate_bindings() {
    Frontend f;
    f.check(R"(
type T = Int
struct T {}
let global = 1
var global = 2
fun f<A, A>(x: Int, x: Int) {
    let local = 1
    let local = 2
    if true { let local = 3 }
}
struct S(x: Int, x: Int) {}
enum E { Case(x: Int, x: Int), Case }
)", {DiagCode::DuplicateBinding, DiagCode::DuplicateBinding, DiagCode::DuplicateBinding,
      DiagCode::DuplicateBinding, DiagCode::DuplicateBinding, DiagCode::DuplicateBinding,
      DiagCode::DuplicateBinding, DiagCode::DuplicateBinding});
    for (const Diagnostic& d : f.diag.all()) {
        CHECK(d.notes.size() == 1);
        CHECK(d.notes[0].span.lo < d.span.lo);
        const auto& text = f.sources.file(d.span.file).text;
        CHECK(text.substr(d.span.lo, d.span.hi - d.span.lo) ==
              text.substr(d.notes[0].span.lo, d.notes[0].span.hi - d.notes[0].span.lo));
    }
}

void declaration_contexts() {
    Frontend f;
    f.check(R"(
init {}
deinit {}
type Missing
type Bounded: Trait = Int
interface I {
    type Item
    type Index: Trait = Int
    class C { type Missing }
}
class C {
    init {}
    init {}
    init(x: Int) {}
    init(x: String) {}
    deinit {}
    deinit {}
}
)", {DiagCode::InvalidDeclarationContext, DiagCode::InvalidDeclarationContext,
      DiagCode::InvalidTypeAlias, DiagCode::InvalidTypeAlias, DiagCode::InvalidTypeAlias,
      DiagCode::InvalidDeclarationContext, DiagCode::InvalidDeclarationContext});
}

void flow_contexts() {
    Frontend f;
    f.check(R"(
let global = if true { return 1 } else { 2 }
fun plain() {
    break
    continue
    throw error
    await task()
    loop { if true { break }; continue }
    break
    while if true { break; true } {}
    for x in if true { continue; items } {}
}
class C { deinit { throw error } }
async fun asynchronous() { await task() }
fun after() { await task() }
)", {DiagCode::InvalidControlFlow, DiagCode::InvalidControlFlow, DiagCode::InvalidControlFlow,
      DiagCode::ThrowOutsideThrows, DiagCode::AwaitOutsideAsync, DiagCode::InvalidControlFlow,
      DiagCode::InvalidControlFlow, DiagCode::InvalidControlFlow, DiagCode::ThrowOutsideThrows,
      DiagCode::AwaitOutsideAsync});
}

void isolated_defaults_and_initializers() {
    Frontend f;
    f.check(R"(
@attr(if true { break })
async fun f(x: Int = await task()) throws(Error) { await task(); throw error }
class C(x: Int = if true { return 1 }) : Base(if true { continue }) {
    let field = if true { return 2 }
    init(x: Int = if true { return 3 }) throws(Error) { throw error }
}
)", {DiagCode::InvalidControlFlow, DiagCode::AwaitOutsideAsync, DiagCode::InvalidControlFlow,
      DiagCode::InvalidControlFlow, DiagCode::InvalidControlFlow, DiagCode::InvalidControlFlow});
}

void patterns() {
    Frontend f;
    f.check(R"(
fun f(x: E) = match x {
    .A(a, a) -> a
    .A(a) | .B(b) -> 0
    .A(a) | .B(a) -> a
    .A(.B(a) | .C(a), a) -> a
    .A(is T as a, a) -> a
    .A(a) | .B(a, b) -> a
    .A(.B(a, b) | .C(b, a)) -> a
    .A(.B(a) | .C(a)) | .D(a) -> a
}
)", {DiagCode::DuplicateBinding, DiagCode::PatternBindingMismatch, DiagCode::DuplicateBinding,
      DiagCode::DuplicateBinding, DiagCode::PatternBindingMismatch});
}

void valid_contexts_and_shadowing() {
    Frontend f;
    f.check(R"(
type Integer = Int
let x = 1
class C<T>(let x: T) {
    type Integer = Int
    init { return }
    init(x: T) throws(Error) { throw error }
    deinit { cleanup() }
    fun f<T>(x: T) {
        let x = 2
        if let x = maybe { let x = 3 }
        while true { for x in xs { continue }; break }
        for x in xs { let x = 4; continue }
        loop { break }
    }
}
fun f(x: Int): Int = x
fun f(x: String): String = x
async fun f() throws(Error) {
    await task()
    throw error
}
fun catches() {
    let x = try? unsafe { throw error }
    let y = try? task()
}
enum E { A(Int), B(value: Int) }
interface I { type Element: Trait; type Index = Int }
)", {});
}

void expression_children() {
    // Each container must visit its children, including both sides of binary
    // expressions, arguments, interpolation segments and match guards.
    for (std::string_view source : {
            "fun f() = (await task())", "fun f() = [await task()]",
            "fun f() = 1 + await task()", "fun f() = (await task()) + 1",
            "fun f() = call(await task())", "fun f() = (await task())()",
            "fun f() = xs[await task()]", "fun f() = (await task())[0]",
            "fun f() = (await task()).x", "fun f() = (await task())?.x",
            "fun f() = (await task())!", "fun f() = (await task()) as Int",
            "fun f() = (await task()).x<Int>()", "fun f() = unsafe { await task() }",
            "fun f() = \"x=\\(await task())\"", "fun f() = if await task() {}",
            "fun f() = match await task() { _ -> 0 }",
            "fun f() = match x { _ if await task() -> 0 }",
            "fun f() = match x { _ -> await task() }",
            "fun f() { defer { await task() } }",
            "fun f() { xs[await task()] = 1 }", "fun f() { x = await task() }",
        }) {
        Frontend f;
        f.check(source, {DiagCode::AwaitOutsideAsync});
    }
    Frontend f;
    f.check("fun f() { let x = try? unsafe { throw error }; throw error }", {DiagCode::ThrowOutsideThrows});
}

void flat_trees_and_multiple_files() {
    Frontend f;
    std::string source = "let sum = 1";
    for (int i = 0; i < 20000; ++i) source += " + 1";
    source += "\nlet chain = value";
    for (int i = 0; i < 20000; ++i) source += ".member";
    source += "\nfun alternatives(x: E) = match x { .A(a)";
    for (int i = 0; i < 2000; ++i) source += " | .B(a)";
    source += " -> a }";
    const auto* first = f.parse(source);
    validate_file(*first, f.names, f.diag);
    CHECK(f.diag.error_count() == 0);
    const auto* invalid = f.parse("fun f() { break }");
    validate_file(*invalid, f.names, f.diag);
    CHECK(f.diag.error_count() == 1);
    const auto* second = f.parse("let sum = 1\nlet chain = 2");
    validate_file(*second, f.names, f.diag);
    CHECK(f.diag.error_count() == 1);
    CHECK(f.diag.all()[0].span.file == invalid->file);
}
}  // namespace

int main() {
    duplicate_bindings();
    declaration_contexts();
    flow_contexts();
    isolated_defaults_and_initializers();
    patterns();
    valid_contexts_and_shadowing();
    expression_children();
    flat_trees_and_multiple_files();
}
