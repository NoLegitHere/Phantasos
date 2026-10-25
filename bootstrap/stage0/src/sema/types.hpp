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

// Built-in identity plus optional wrappers, without recursive type storage.
// Int/UInt use the initial x86_64 ranges and stay distinct from Int64/UInt64.
// Deferred means this pass cannot establish a type, not success.
struct ScalarType {
    enum Kind : u8 {
        Deferred, Error, Int8, Int16, Int32, Int64, UInt8, UInt16, UInt32, UInt64,
        Int, UInt, Float32, Float64, Bool, Char, String, Unit, Never
    };
    Kind kind = Deferred;
    u32 optional_depth = 0;
    constexpr ScalarType(Kind value = Deferred, u32 depth = 0) : kind(value), optional_depth(depth) {}
    constexpr ScalarType scalar() const { return {kind}; }
    constexpr ScalarType optional() const {
        return kind == Deferred || kind == Error ? ScalarType{kind} : ScalarType{kind, optional_depth + 1};
    }
    constexpr ScalarType element() const { return optional_depth ? ScalarType{kind, optional_depth - 1} : ScalarType{}; }
    friend bool operator==(ScalarType, ScalarType) = default;
};

std::string scalar_type_name(ScalarType type);

struct ScalarSignature {
    std::vector<ScalarType> parameters;
    ScalarType result = ScalarType::Deferred;
    bool generic = false;
};

enum class CallState : u8 { Resolved, Deferred, Error };
enum class CallKind : u8 { Function, NumericConversion };
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

private:
    friend class ScalarChecker;
    std::unordered_map<const ast::Expr*, ScalarType> expressions_;
    std::vector<ScalarType> bindings_;
    std::unordered_map<usize, ScalarSignature> signatures_;
    std::unordered_map<const ast::CallExpr*, CheckedCall> calls_;
    std::unordered_map<const ast::CallExpr*, CheckedConversion> conversions_;
};

// Run only after successful validation and resolution. Checks scalar literals,
// bindings, operators, conditions, defaults, explicit returns and assignments.
// Also infers expression-body returns and checks direct file-level function
// calls, numeric conversions and optional wrappers on built-in facts.
// Generic/member/indirect calls, nominal/composite types beyond those wrappers,
// aliases, effects and ownership stay deferred.
// Dependency evaluation and AST traversal use explicit stacks, including
// forward binding inference. Cyclic inference remains deferred for a later pass.
[[nodiscard]] ScalarTypes check_scalar_types(const ast::File& file, const Resolution& resolution,
                                             const Interner& names, DiagEngine& diag);

}  // namespace phs
