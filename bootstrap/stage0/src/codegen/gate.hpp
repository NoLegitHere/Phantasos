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

#include "sema/types.hpp"

namespace phs {

// Accepts the subset that stage0 code generation lowers: non-generic top-level
// functions over Bool and built-in integer types, with let/var locals,
// assignment, if/while/loop/break/continue/return, direct calls, arithmetic,
// comparisons, short-circuit logic and unary -, ! and ~. Everything else, and
// any expression whose type is not known, reports E0600. A program needs one
// `fun main()` with no parameters that returns Int or Unit (E0601).
// Run after check succeeds; the AST, Resolution and ScalarTypes are unchanged.
// Returns true when the file is inside the subset.
bool check_codegen_subset(const ast::File& file, const Resolution& resolution, const ScalarTypes& types,
                          const Interner& names, DiagEngine& diag);

}  // namespace phs
