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

#include <string>

#include "sema/resolve.hpp"

namespace phs {

// Built-in identity or a canonical composite/nominal descriptor, plus optional
// wrappers. Descriptor IDs belong to one ScalarTypes result, never another file.
// Int/UInt use the initial x86_64 ranges and stay distinct from Int64/UInt64.
// Deferred means this pass cannot establish a type, not success.
struct ScalarType {
    enum Kind : u8 {
        Deferred, Error, Int8, Int16, Int32, Int64, UInt8, UInt16, UInt32, UInt64,
        Int, UInt, Float32, Float64, Bool, Char, String, Unit, Never,
        Nominal, Array, Pointer, Shared, Weak, Function
    };
    Kind kind = Deferred;
    u32 optional_depth = 0;
    usize detail = std::numeric_limits<usize>::max(); // descriptor ID in this result's type table
    constexpr ScalarType(Kind value = Deferred, u32 depth = 0, usize descriptor = std::numeric_limits<usize>::max())
        : kind(value), optional_depth(depth), detail(descriptor) {}
    constexpr ScalarType scalar() const { auto result = *this; result.optional_depth = 0; return result; }
    constexpr ScalarType optional() const {
        return kind == Deferred || kind == Error ? ScalarType{kind} : ScalarType{kind, optional_depth + 1, detail};
    }
    constexpr ScalarType element() const { return optional_depth ? ScalarType{kind, optional_depth - 1, detail} : ScalarType{}; }
    friend bool operator==(ScalarType, ScalarType) = default;
};

std::string scalar_type_name(ScalarType type);

struct TypeDescriptor {
    ScalarType::Kind kind = ScalarType::Deferred;
    ScalarType element;
    std::vector<ScalarType> parameters;
    ScalarType result;
    ScalarType error;
    BindingId declaration;
    bool mutable_pointer = false;
    bool throws = false;
    friend bool operator==(const TypeDescriptor&, const TypeDescriptor&) = default;
};
struct TypeDescriptorHash {
    usize operator()(const TypeDescriptor& descriptor) const;
};
class TypeTable {
public:
    ScalarType intern(TypeDescriptor descriptor);
    // During construction, interning may invalidate descriptor pointers. A
    // completed ScalarTypes result is immutable and exposes stable pointers.
    const TypeDescriptor* descriptor(ScalarType type) const {
        return type.detail == std::numeric_limits<usize>::max() ? nullptr : &descriptors_.at(type.detail);
    }
    std::string name(ScalarType type, const Resolution& resolution, const Interner& names) const;
private:
    std::vector<TypeDescriptor> descriptors_;
    std::unordered_map<TypeDescriptor, usize, TypeDescriptorHash> ids_;
};

enum class ExpansionState : u8 { Resolved, Deferred, Error };
struct ExpandedType {
    ExpansionState state = ExpansionState::Deferred;
    BindingId target; // terminal declaration identity, not the alias's identity
    ScalarType scalar; // historical name: includes normalized composite facts
    u32 optional_depth = 0; // retained even for nominal targets
    const ast::TypeExpr* syntax = nullptr; // eligible transparent RHS, for composite type normalization
};
struct TypeExpansion {
    ExpandedType type;
    ast::Name unknown_member;
    BindingId member_owner;
};

class AliasExpander;
// Owns expansion facts, with IDs belonging to the supplied Resolution. Keep
// that result, its AST and interner alive when querying type syntax/entities.
// Queries are pure; unknown_member lets the caller diagnose an unresolved
// direct nominal member without duplicating a declaration's existing error.
class AliasExpansion {
public:
    const ExpandedType* alias(BindingId id) const {
        const auto it = aliases_.find(id.value);
        return it == aliases_.end() ? nullptr : &it->second;
    }
    TypeExpansion type(const ast::TypeExpr* syntax, const Resolution& resolution) const;
    ExpandedType entity(const ast::Expr* syntax, const Resolution& resolution) const;

private:
    friend class AliasExpander;
    friend class ScalarChecker;
    struct Lookup {
        ExpandedType type;
        BindingId pending;
        ast::Name unknown;
        BindingId owner;
    };
    Lookup binding(BindingId id, const Resolution& resolution) const;
    Lookup path(std::span<const ast::Name> parts, NameSpace space, const Resolution& resolution) const;
    Lookup lookup(const ast::TypeExpr* syntax, const Resolution& resolution) const;
    std::unordered_map<usize, ExpandedType> aliases_;
    std::unordered_map<usize, ScalarType> builtins_;
    std::unordered_map<usize, std::unordered_map<u32, BindingId>> member_types_;
    std::unordered_map<usize, std::vector<u32>> member_values_;
    std::vector<u8> progress_;
    BindingId unit_;
};

// Non-generic transparent aliases; associated-type requirements stay deferred.
// Both dependency traversal and wrapper traversal are iterative. Requires a
// successful resolution, and leaves the AST/Resolution unchanged.
[[nodiscard]] AliasExpansion expand_aliases(const Resolution& resolution, const Interner& names, DiagEngine& diag);

struct ScalarSignature {
    std::vector<ScalarType> parameters;
    ScalarType result = ScalarType::Deferred;
    bool generic = false;
};

enum class CallState : u8 { Resolved, Deferred, Error };
enum class CallKind : u8 { Function, NumericConversion, NominalConstructor, IndirectFunction };
enum class ConversionKind : u8 { Widening, Checked, Truncating };
struct CheckedConversion {
    CallState state = CallState::Deferred;
    ConversionKind kind = ConversionKind::Widening;
    ScalarType source;
    ScalarType destination;
};
// Resolved means the argument-based target was selected. Return typing,
// effects, conventions and ownership can still require later checking.
struct CheckedCall {
    CallState state = CallState::Deferred;
    BindingId target;
    // Maps each supplied argument to a parameter; omitted defaults have no entry.
    std::vector<usize> parameters;
    CallKind kind = CallKind::Function;
};

class ScalarChecker;

// Owns type facts, without modifying the AST or Resolution. Expression keys
// refer to the input AST; keep it alive. Binding IDs belong to that Resolution.
// Missing/unsupported facts return Deferred. Error marks a diagnosed expression.
class ScalarTypes {
public:
    ScalarType expression(const ast::Expr* expr) const {
        const auto it = expressions_.find(expr);
        return it == expressions_.end() ? ScalarType::Deferred : it->second;
    }
    ScalarType binding(BindingId id) const { return bindings_.at(id.value); }
    const ScalarSignature* signature(BindingId id) const {
        const auto it = signatures_.find(id.value);
        return it == signatures_.end() ? nullptr : &it->second;
    }
    const CheckedCall* call(const ast::CallExpr* expr) const {
        const auto it = calls_.find(expr);
        return it == calls_.end() ? nullptr : &it->second;
    }
    const CheckedConversion* conversion(const ast::CallExpr* expr) const {
        const auto it = conversions_.find(expr);
        return it == conversions_.end() ? nullptr : &it->second;
    }
    const ExpandedType* alias(BindingId id) const { return aliases_.alias(id); }
    const TypeDescriptor* descriptor(ScalarType type) const { return types_.descriptor(type); }
    std::string type_name(ScalarType type, const Resolution& resolution, const Interner& names) const {
        return types_.name(type, resolution, names);
    }

private:
    friend class ScalarChecker;
    std::unordered_map<const ast::Expr*, ScalarType> expressions_;
    std::vector<ScalarType> bindings_;
    std::unordered_map<usize, ScalarSignature> signatures_;
    std::unordered_map<const ast::CallExpr*, CheckedCall> calls_;
    std::unordered_map<const ast::CallExpr*, CheckedConversion> conversions_;
    AliasExpansion aliases_;
    TypeTable types_;
};

// Run only after successful validation and resolution. Checks scalar literals,
// bindings, operators, conditions, defaults, explicit returns and assignments.
// Also infers expression-body returns and checks direct file-level function
// calls, numeric conversions and optional wrappers on built-in facts.
// Includes fully known composites, non-generic nominal identities, arrays,
// primary constructors and calls through known function types. Generic aliases,
// member lookup, inheritance, function-value inference, effects and ownership
// still require later passes.
// Dependency evaluation and AST traversal use explicit stacks, including
// forward binding inference. Cyclic inference remains deferred for a later pass.
[[nodiscard]] ScalarTypes check_scalar_types(const ast::File& file, const Resolution& resolution,
                                             const Interner& names, DiagEngine& diag);

}  // namespace phs
