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
#include <array>
#include <format>
#include <initializer_list>
#include <string>
#include <utility>
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
        const auto id = session.sources().add("calls.phs", source);
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

    BindingId binding(std::string_view name, BindingKind kind = BindingKind::Variable, usize occurrence = 0) const {
        for (usize i = 0; i < result.resolution->bindings().size(); ++i) {
            const auto& b = result.resolution->bindings()[i];
            if (b.kind == kind && session.names().str(b.name.sym) == name) {
                if (!occurrence) return BindingId{i};
                --occurrence;
            }
        }
        CHECK(false);
        return {};
    }
    Type type(std::string_view name) const { return result.scalar_types->binding(binding(name)); }
    const ScalarSignature& signature(std::string_view name, usize occurrence = 0) const {
        const auto* sig = result.scalar_types->signature(binding(name, BindingKind::Function, occurrence));
        CHECK(sig);
        return *sig;
    }
    const ast::CallExpr* expr(std::string_view name) const {
        const auto* var = ast::cast<ast::VarItem>(result.resolution->binding(binding(name)).item);
        return ast::cast<ast::CallExpr>(var->init);
    }
    const CheckedCall& call(std::string_view name) const {
        const auto* call = result.scalar_types->call(expr(name));
        CHECK(call);
        return *call;
    }
};

void signatures_inference_and_mapping() {
    Frontend f;
    f.check(R"(
let forward = later(2)
fun later(value: Int) = value + 1
fun defaults(first: Int = 1, second: Int = first): Int = first + second
let named = defaults(second: 2, first: 3)
let omitted = defaults(second: 4)
let paren = (later)(5)
fun wide(value: UInt64): UInt64 = value
let maximum = wide(18446744073709551615)
extern "C" { fun foreign(value: Float32): Float32 }
let external = foreign(1.0)
fun nothing() {}
let unit = nothing()
fun inferredFloat() = 1.0
let fraction = inferredFloat()
fun inferredChar() = 'a'
let character = inferredChar()
)");
    CHECK(f.signature("later").parameters == std::vector<Type>{Type::Int});
    CHECK(f.signature("later").result == Type::Int && !f.signature("later").generic);
    CHECK(f.signature("nothing").result == Type::Unit);
    CHECK(f.type("forward") == Type::Int && f.type("named") == Type::Int && f.type("omitted") == Type::Int);
    CHECK(f.type("paren") == Type::Int && f.type("maximum") == Type::UInt64);
    CHECK(f.type("external") == Type::Float32 && f.type("unit") == Type::Unit);
    CHECK(f.type("fraction") == Type::Float64 && f.type("character") == Type::Char);
    CHECK(f.call("named").state == CallState::Resolved);
    CHECK(f.call("named").parameters == std::vector<usize>({1, 0}));
    CHECK(f.call("omitted").parameters == std::vector<usize>{1});
    CHECK(f.call("forward").target == f.binding("later", BindingKind::Function));
    CHECK(f.result.scalar_types->expression(f.expr("maximum")->args[0].value) == Type::UInt64);
}

void shape_and_type_errors() {
    for (std::string_view invocation : {
        "f()", "f(true)", "f(1, 2)", "f(1, true, 3)", "f(missing: 1)",
        "f(second: true)", "f(first: 1, 2)", "f(1, first: 2)", "f(first: 1, first: 2)"
    }) {
        Frontend f;
        f.check(std::format("fun f(first: Int, second: Bool = false): Int = first\nlet bad = {}", invocation),
                {DiagCode::NoMatchingCall});
        CHECK(f.call("bad").state == CallState::Error && !f.call("bad").target.valid());
        CHECK(f.session.diagnostics().all().front().notes.size() == 1);
    }
    Frontend zero;
    zero.check("fun f() {}\nlet bad = f(missing: 1)", {DiagCode::NoMatchingCall});
    Frontend wrong_result;
    wrong_result.check("fun f() = 1\nlet bad: Bool = f()", {DiagCode::ScalarTypeMismatch});
    Frontend widening;
    widening.check("fun f(x: Int64): Int64 = x\nlet x = 1\nlet bad = f(x)", {DiagCode::NoMatchingCall});
    Frontend default_range;
    default_range.check("fun f(x: Int8 = 128): Int8 = x\nlet a = f()\nlet b = f()", {DiagCode::ScalarLiteralRange});
}

void overloads_and_literal_ranges() {
    Frontend f;
    f.check(R"(
fun pick(value: Int): Int = value
fun pick(value: String): String = value
let integer = pick(1)
let string = pick("hello")
fun width(value: Int8): Int8 = value
fun width(value: Int64): Int64 = value
let large = width(500)
let small: Int8 = 1
let typed = width(small)
fun native(value: Int): Int = value
fun native(value: Int64): Int64 = value
fun native(value: UInt64): UInt64 = value
let preferred = native(1)
let maximum = native(18446744073709551615)
)");
    CHECK(f.type("integer") == Type::Int && f.type("string") == Type::String);
    CHECK(f.type("large") == Type::Int64 && f.type("typed") == Type::Int8);
    CHECK(f.type("preferred") == Type::Int && f.type("maximum") == Type::UInt64);
    CHECK(f.call("large").target == f.binding("width", BindingKind::Function, 1));
    CHECK(f.result.scalar_types->expression(f.expr("large")->args[0].value) == Type::Int64);

    Frontend ambiguous;
    ambiguous.check("fun f(x: Int8): Int8 = x\nfun f(x: UInt8): UInt8 = x\nlet bad = f(1)", {DiagCode::AmbiguousCall});
    CHECK(ambiguous.call("bad").state == CallState::Error && !ambiguous.call("bad").target.valid());
    Frontend default_tie;
    default_tie.check("fun f(x: Int) {}\nfun f(x: Int, y: Int = 0) {}\nlet bad = f(1)", {DiagCode::AmbiguousCall});
    for (std::string_view expr : {"128", "-129", "(100 + 128)"}) {
        Frontend range;
        range.check(std::format("fun f(x: Int8): Int8 = x\nlet bad = f({})", expr), {DiagCode::ScalarLiteralRange});
        CHECK(range.call("bad").state == CallState::Error && range.call("bad").target.valid());
    }
    Frontend minimum;
    minimum.check("fun f(x: Int8): Int8 = x\nlet good = f(-(128))");
    CHECK(minimum.type("good") == Type::Int8);
}

void deferred_and_noncallable() {
    Frontend deferred;
    deferred.check(R"(
fun f(x: Int): Int = x
fun f<T: Copy>(x: T): T = x
let genericCandidate = f(1)
fun opaque<T>(): Int = 1
fun concrete(x: Int): Int = x
let unknownArgument = concrete(opaque())
type Alias = Int
fun alias(x: Alias): Int = 1
let aliasArgument = alias(1)
let saved = concrete
let indirect = saved(1)
class Box { static fun make(): Int = 1 }
let member = Box.make()
let conversion = Int64(1)
interface Marker {}
fun constrained(value: Int): Int where Int: Copy = value
let constraint = constrained(1)
)");
    for (std::string_view name : {"genericCandidate", "unknownArgument", "indirect", "constraint"}) {
        CHECK(deferred.call(name).state == CallState::Deferred && !deferred.call(name).target.valid());
        CHECK(deferred.type(name) == Type::Deferred);
    }
    CHECK(deferred.type("aliasArgument") == Type::Int && deferred.call("aliasArgument").state == CallState::Resolved);
    CHECK(deferred.type("member") == Type::Int && deferred.call("member").kind == CallKind::Method);
    CHECK(deferred.type("conversion") == Type::Int64);
    CHECK(deferred.call("conversion").kind == CallKind::NumericConversion);
    CHECK(deferred.result.scalar_types->expression(deferred.expr("genericCandidate")->args[0].value) == Type::Deferred);
    Frontend shape_eliminated;
    shape_eliminated.check("fun f(x: Int): Int = x\nfun f<T>(x: T, y: Int): T = x\nlet value = f(1)");
    CHECK(shape_eliminated.type("value") == Type::Int);
    Frontend independent;
    independent.check("fun opaque<T>(): Int = 1\nfun f(a: Int, b: String) {}\nlet bad = f(opaque(), false)",
                      {DiagCode::NoMatchingCall});
    for (std::string_view expr : {"1()", "true()", "(1)()", "f()()"}) {
        Frontend bad;
        bad.check(std::format("fun f(): Int = 1\nlet bad = {}", expr), {DiagCode::NotCallable});
    }
    Frontend cascade;
    cascade.check("fun f(x: Int): Int = x\nlet bad = f(true + 1)", {DiagCode::InvalidScalarOperand});
    CHECK(cascade.call("bad").state == CallState::Error);
    Frontend scalar_binding;
    scalar_binding.check("let value = true\nlet bad = value()", {DiagCode::NotCallable});
}

void prelude_print() {
    Frontend prints;
    prints.check(R"(
let text: String = "hi"
let flag = true
let n: UInt = 7
let a = print(text)
let b = print("literal")
let c = print(1)
let d = print(value: n)
let e = print(flag && false)
let f = print(-5 * 2)
let g = print(if flag { 1 } else { 2 })
let h = print(if flag { n } else { 2 })
let i = print(if flag { "yes" } else { text })
let j = print(match flag { true -> false
    _ -> flag })
)");
    const std::pair<std::string_view, Type> expected[] = {
        {"a", Type::String}, {"b", Type::String}, {"c", Type::Int}, {"d", Type::UInt}, {"e", Type::Bool}, {"f", Type::Int},
        {"g", Type::Int}, {"h", Type::UInt}, {"i", Type::String}, {"j", Type::Bool}};
    for (const auto& [name, parameter] : expected) {
        const auto& call = prints.call(name);
        CHECK(call.state == CallState::Resolved && call.kind == CallKind::Builtin);
        CHECK(call.parameters == std::vector<usize>{0} && call.parameter_types == std::vector<Type>{parameter});
        CHECK(prints.type(name) == Type::Unit);
        // As for ordinary call arguments, a branch is typed through its tails, not itself.
        CHECK(name >= "g" || prints.result.scalar_types->expression(prints.expr(name)->args[0].value) == parameter);
    }
    CHECK(prints.call("a").target == prints.binding("print", BindingKind::BuiltinValue));

    for (std::string_view bad : {"print()", "print(1, 2)", "print(text: \"a\")", "print(Int8(1))", "print(1.5)",
                                 "print(UInt64(1))", "print([1])", "print(print(1))", "print(maybe)",
                                 "print(if true { Int8(1) } else { Int8(2) })", "print(if true { 1 })",
                                 "print(if true { 1 } else { \"a\" })", "print(if true { 1.5 } else { 2.5 })",
                                 "print(none)", "print((none))", "print(if true { none } else { 1 })"}) {
        Frontend rejected;
        rejected.check(std::format("let maybe: Int? = none\nlet bad = {}", bad), {DiagCode::NoMatchingCall});
        CHECK(rejected.call("bad").state == CallState::Error && rejected.call("bad").kind == CallKind::Builtin);
    }

    // A declaration named print shadows the prelude function.
    Frontend shadowed;
    shadowed.check("fun print(x: Int): Int = x\nlet value = print(3)");
    CHECK(shadowed.call("value").kind == CallKind::Function && shadowed.type("value") == Type::Int);
    Frontend local;
    local.check("let print = 3\nlet bad = print(1)", {DiagCode::NotCallable});
}

void recursion_and_stable_results() {
    Frontend f;
    f.check(R"(
fun recursive(value: Int): Int = recursive(value)
let result = recursive(1)
fun a() = b()
fun b() = a()
let cyclic = a()
)");
    CHECK(f.type("result") == Type::Int && f.type("cyclic") == Type::Deferred);
    CHECK(f.signature("a").result == Type::Deferred && f.signature("b").result == Type::Deferred);
    const auto* call = &f.call("result");
    const auto* signature = &f.signature("recursive");
    for (int i = 0; i < 128; ++i) {
        const auto id = f.session.sources().add("another", "fun identity(x: Int) = x\nlet value = identity(1)");
        CHECK(id && f.session.check(*id).scalar_types && !f.session.diagnostics().has_errors(*id));
    }
    CHECK(&f.call("result") == call && &f.signature("recursive") == signature);
    CHECK(call->state == CallState::Resolved && signature->result == Type::Int);
}

void eliminated_overloads_do_not_create_return_cycles() {
    const std::array<std::string_view, 3> declarations = {
        "fun a() = b(1)\n", "fun b(x: Bool) = a()\n", "fun b(x: Int) = 1\n"
    };
    std::array<usize, 3> order{0, 1, 2};
    do {
        for (int placement = 0; placement < 3; ++placement) {
            std::string source;
            for (usize index : order) source += declarations[index];
            if (placement == 0) source += "let first = a()\n";
            source += "let bad: Bool = b(true)\n";
            if (placement == 1) source += "let first = a()\n";
            Frontend f;
            f.check(source, {DiagCode::ScalarTypeMismatch});
            CHECK(f.signature("a").result == Type::Int);
            CHECK(f.signature("b", 0).result == Type::Int && f.signature("b", 1).result == Type::Int);
            CHECK(f.type("bad") == Type::Error && f.call("bad").state == CallState::Resolved);
            if (placement != 2) CHECK(f.type("first") == Type::Int);
        }
    } while (std::next_permutation(order.begin(), order.end()));

    // Exclude recursive overloads by arity, literal preference and range too.
    for (std::string_view source : {
        "fun a() = b(1)\nfun b(x: Int, extra: Int) = a()\nfun b(x: Int) = x\n"
        "let first = a()\nlet bad: Bool = b(1, 2)",
        "fun a() = b(1)\nfun b(x: Int8) = a()\nfun b(x: Int) = x\n"
        "let first = a()\nlet small: Int8 = 1\nlet bad: Bool = b(small)",
        "fun a() = b(500)\nfun b(x: Int8) = a()\nfun b(x: Int64) = x\n"
        "let first = a()\nlet small: Int8 = 1\nlet bad: Bool = b(small)"
    }) {
        Frontend f;
        f.check(source, {DiagCode::ScalarTypeMismatch});
        CHECK(f.signature("a").result != Type::Deferred);
        CHECK(f.signature("b", 0).result != Type::Deferred && f.signature("b", 1).result != Type::Deferred);
    }
}

void long_forward_call_chain() {
    Frontend f;
    std::string source = "let first = f0()\n";
    for (int i = 0; i < 10000; ++i) source += std::format("fun f{}() = f{}()\n", i, i + 1);
    source += "fun f10000() = 1";
    f.check(source, {}, false);
    CHECK(f.type("first") == Type::Int && f.signature("f0").result == Type::Int);
}
}  // namespace

int main() {
    signatures_inference_and_mapping();
    shape_and_type_errors();
    overloads_and_literal_ranges();
    deferred_and_noncallable();
    prelude_print();
    recursion_and_stable_results();
    eliminated_overloads_do_not_create_return_cycles();
    long_forward_call_chain();
}
