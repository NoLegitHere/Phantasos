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

#include "sema/resolve.hpp"

namespace phs {

// Int/UInt describe the initial x86_64 stage0 targets and remain distinct from
// Int64/UInt64. Deferred means this pass cannot establish a type, not success.
enum class ScalarType : u8 {
    Deferred, Error, Int8, Int16, Int32, Int64, UInt8, UInt16, UInt32, UInt64,
    Int, UInt, Float32, Float64, Bool, Char, String, Unit, Never
};

std::string_view scalar_type_name(ScalarType type);

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

private:
    friend class ScalarChecker;
    std::unordered_map<const ast::Expr*, ScalarType> expressions_;
    std::vector<ScalarType> bindings_;
};

// Run only after successful validation and resolution. Checks scalar literals,
// bindings, operators, conditions, defaults, explicit returns and assignments.
// Calls, nominal/composite types, aliases, effects and ownership stay deferred.
// Dependency evaluation and AST traversal use explicit stacks, including
// forward binding inference. Cyclic inference remains deferred for a later pass.
[[nodiscard]] ScalarTypes check_scalar_types(const ast::File& file, const Resolution& resolution,
                                             const Interner& names, DiagEngine& diag);

}  // namespace phs
