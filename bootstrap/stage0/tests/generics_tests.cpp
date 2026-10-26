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

#include "frontend/session.hpp"
#include "syntax/ast_dump.hpp"

using namespace phs;
using Type = ScalarType;

namespace {
struct Frontend {
    FrontendSession session;
    CheckResult result{};
    void check(std::string_view source, std::initializer_list<DiagCode> expected = {}, bool dump = true) {
        const auto id = session.sources().add("generics.phs", source); CHECK(id);
        const auto* syntax = session.parse(*id);
        if (session.diagnostics().has_errors(*id)) session.diagnostics().render(stderr);
        CHECK(!session.diagnostics().has_errors(*id));
        const auto before = dump ? dump_ast(*syntax, session.names()) : std::string{};
        result = session.check(*id); CHECK(result.scalar_types);
        if (dump) CHECK(dump_ast(*syntax, session.names()) == before);
        std::vector<DiagCode> actual, wanted(expected);
        for (const auto& d : session.diagnostics().all()) actual.push_back(d.code);
        std::ranges::sort(actual); std::ranges::sort(wanted);
        if (actual != wanted) session.diagnostics().render(stderr);
        CHECK(actual == wanted);
        const auto errors = session.diagnostics().error_count();
        CHECK(session.check(*id).scalar_types == result.scalar_types && session.diagnostics().error_count() == errors);
    }
    BindingId id(std::string_view name, BindingKind kind = BindingKind::Variable) const {
        for (usize i = 0; i < result.resolution->bindings().size(); ++i) {
            const auto& b = result.resolution->bindings()[i];
            if (b.kind == kind && session.names().str(b.name.sym) == name) return BindingId{i};
        }
        CHECK(false); return {};
    }
    Type value(std::string_view name) const { return result.scalar_types->binding(id(name)); }
    const ast::CallExpr* invocation(std::string_view name) const {
        return ast::cast<ast::CallExpr>(ast::cast<ast::VarItem>(result.resolution->binding(id(name)).item)->init);
    }
    const CheckedCall& call(std::string_view name) const {
        const auto* record = result.scalar_types->call(invocation(name)); CHECK(record); return *record;
    }
    std::string name(Type type) const { return result.scalar_types->type_name(type, *result.resolution, session.names()); }
};

void applications_and_substituted_members() {
    Frontend f;
    f.check(R"(
type List<T> = [T]
type Rows<T> = List<List<T>>
type Identity<T> = T
type Applied = Identity<Int8>
type Callable<T, E> = fun([T?], *mut E) -> shared T throws(E)
class Box<T>(let value: T) {
    type Values = [T]
    fun get(): T = value
    fun take(x: T): T = x
    fun other<U>(x: U): U = x
    fun shadow<T>(x: T): T = x
    fun values(x: Values): Values = x
    fun itself(): Self = self
    static fun staticValue<U>(x: U): U = x
}
type Wrapped<T> = Box<List<T>>
let number = Box<Int8>(1)
let flag = Box<Bool>(true)
let field = number.value
let flagField = flag.value
let method = number.get()
let argument = number.take(2)
let other = number.other<Bool>(true)
let shadowed = number.shadow<Bool>(true)
let values = number.values([1, 2])
let itself = number.itself()
let statically = Box<Int>.staticValue<Bool>(true)
let wrapped: Wrapped<Int> = Wrapped<Int>([1])
let items = wrapped.value
let rows: Rows<Int8> = [[1], []]
let callback: Callable<Int, Bool>
let converted = Identity<Int8>(1)
let applied: Applied = 1
)");
    CHECK(f.name(f.value("number")) == "Box<Int8>" && f.name(f.value("flag")) == "Box<Bool>");
    CHECK(f.value("field") == Type::Int8 && f.value("flagField") == Type::Bool);
    CHECK(f.value("method") == Type::Int8 && f.value("argument") == Type::Int8);
    CHECK(f.value("other") == Type::Bool && f.value("statically") == Type::Bool);
    CHECK(f.value("shadowed") == Type::Bool && f.name(f.value("values")) == "[Int8]");
    CHECK(f.value("itself") == f.value("number"));
    CHECK(f.name(f.value("wrapped")) == "Box<[Int]>" && f.name(f.value("items")) == "[Int]");
    CHECK(f.name(f.value("rows")) == "[[Int8]]");
    CHECK(f.name(f.value("callback")) == "fun([Int?], *mut Bool) -> shared Int throws(Bool)");
    CHECK(f.value("converted") == Type::Int8 && f.value("applied") == Type::Int8);
    CHECK(f.call("other").type_arguments == std::vector<Type>{Type::Bool});
    CHECK(f.call("argument").parameter_types == std::vector<Type>{Type::Int8});
    CHECK(f.result.scalar_types->descriptor(f.value("number"))->arguments == std::vector<Type>{Type::Int8});
}

void explicitly_applied_functions() {
    Frontend f;
    f.check(R"(
fun identity<T>(x: T): T = x
fun elements<T>(x: [T]): [T] = x
fun twice<T, U>(first: T, second: U): T = first
fun overload<T>(x: T): T = x
fun overload<T, U>(x: T): Bool = true
fun constant<T>() = 1
let integer = identity<Int8>(1)
let boolean = identity<Bool>(true)
let lifted: Int? = 1
let optional = identity<Int?>(lifted)
let array = elements<Int8>([1, 2])
let reordered = twice<Int, Bool>(second: true, first: 1)
let overloadOne = overload<Int>(1)
let overloadTwo = overload<Int, Bool>(1)
let inferredReturn = constant<Int>()
)");
    CHECK(f.value("integer") == Type::Int8 && f.value("boolean") == Type::Bool);
    CHECK(f.value("optional") == Type{Type::Int}.optional() && f.name(f.value("array")) == "[Int8]");
    CHECK(f.value("reordered") == Type::Int && f.call("reordered").parameters == (std::vector<usize>{1, 0}));
    CHECK(f.value("overloadOne") == Type::Int && f.value("overloadTwo") == Type::Bool);
    CHECK(f.value("inferredReturn") == Type::Int);
    const auto* signature = f.result.scalar_types->signature(f.id("identity", BindingKind::Function));
    CHECK(signature && signature->generic && signature->parameters == std::vector<Type>{Type::Deferred});
    CHECK(signature->result == Type::Deferred); // applications never overwrite declaration facts
    const auto* retained = f.result.scalar_types->call(f.invocation("integer"));
    for (int i = 0; i < 128; ++i) {
        const auto file = f.session.sources().add("other.phs", "let value = 1"); CHECK(file);
        CHECK(f.session.check(*file).scalar_types);
    }
    CHECK(f.result.scalar_types->call(f.invocation("integer")) == retained);
}

void rejected_applications() {
    for (const auto& [source, code] : std::vector<std::pair<std::string_view, DiagCode>>{
        {"class Box<T> {}\nlet x: Box<Int, Bool>", DiagCode::GenericArguments},
        {"class Box<T> {}\nlet x = Box<Int, Bool>()", DiagCode::GenericArguments},
        {"let x: Int<Bool>", DiagCode::GenericArguments},
        {"fun f<T>(x: T<Int>) {}", DiagCode::GenericArguments},
        {"type Id<T> = T\nlet x: Id<Int, Bool>", DiagCode::GenericArguments},
        {"fun f<T>(x: T): T = x\nlet x = f<Int, Bool>(1)", DiagCode::GenericArguments},
        {"fun f(x: Int): Int = x\nlet x = f<Int>(1)", DiagCode::GenericArguments},
        {"fun f<T>(x: T): T = x\nlet x = f<Int>(true)", DiagCode::NoMatchingCall},
        {"fun f<T>(x: T): T = x\nlet x = f<Int8>(128)", DiagCode::ScalarLiteralRange},
        {"fun f<T>(x: T): T = x\nlet x: Bool = f<Int>(1)", DiagCode::ScalarTypeMismatch},
        {"class Box<T>(let value: T) {}\nlet x = Box<Int>(true)", DiagCode::NoMatchingCall},
        {"class Box<T>(let value: T) {}\nlet x: Box<Int> = Box<Bool>(true)", DiagCode::ScalarTypeMismatch},
        {"class Box<T>(let value: T) {}\nlet x = Box<Int>(1).missing", DiagCode::UnknownMember},
        {"type A<T> = A<T>\nlet x: A<Int>", DiagCode::AliasCycle},
        {"type A<T> = B<[T]>\ntype B<U> = A<U>\nlet x: A<Int>", DiagCode::AliasCycle},
        {"type A<T> = Box<A<[T]>>\nclass Box<T> {}\nlet x: A<Int>", DiagCode::AliasCycle}
    }) { Frontend bad; bad.check(source, {code}); }
}

void pending_contracts() {
    Frontend f;
    f.check(R"(
interface I { fun read(): Int }
class Box<T>(let value: T) where T: I {}
let constrained = Box<Int>(1)
fun bound<T: Copy>(x: T): T = x
let bounded = bound<Int>(1)
fun inferred<T>(x: T): T = x
let inference = inferred(1)
let noneArgument = inferred<Int?>(none)
fun dependentDefault<T>(x: T = true): T = x
let defaulted = dependentDefault<Int>()
class Owner<T> { class Nested {} }
let captured: Owner.Nested
class Base<T> { let value: T }
class Derived<T>: Base<T> {}
let inherited: Derived<Int>
let dependent = inherited.value
)");
    for (std::string_view name : {"constrained", "bounded", "inference", "noneArgument", "defaulted", "captured", "dependent"})
        CHECK(f.value(name) == Type::Deferred);
}

void iterative_alias_graphs() {
    Frontend f;
    std::string source;
    for (int i = 0; i < 10000; ++i) source += std::format("type A{}<T> = A{}<T>\n", i, i + 1);
    source += "type A10000<T> = [T]\nlet values: A0<Int8> = [1]\n";
    f.check(source, {}, false);
    CHECK(f.name(f.value("values")) == "[Int8]");
    Frontend shared;
    source.clear();
    for (int i = 0; i < 32; ++i) source += std::format("type B{}<T> = fun(B{}<T>) -> B{}<T>\n", i, i + 1, i + 1);
    source += "type B32<T> = T\nlet value: B0<Int>\n";
    shared.check(source, {}, false);
    CHECK(shared.value("value").kind == Type::Function);
    CHECK(shared.name(shared.value("value")).ends_with("..."));
}
} // namespace

int main() {
    applications_and_substituted_members();
    explicitly_applied_functions();
    rejected_applications();
    pending_contracts();
    iterative_alias_graphs();
}
