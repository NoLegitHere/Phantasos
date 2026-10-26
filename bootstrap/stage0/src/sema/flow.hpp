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

// Flow-sensitive checks for callable bodies: every path of a block-bodied
// function with a non-Unit result returns or diverges (E0500), locals declared
// without an initializer are definitely initialized before use (E0501), and
// assignments only target `var` locals/globals, `inout` parameters or a `let`
// local that is still definitely unassigned (E0502). Fields, receivers and
// ownership remain later work. Run after scalar typing succeeds; the AST,
// Resolution and ScalarTypes are unchanged.
void check_flow(const ast::File& file, const Resolution& resolution, const ScalarTypes& types,
                const Interner& names, DiagEngine& diag);

}  // namespace phs
