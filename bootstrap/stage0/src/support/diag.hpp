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

#include <cstdio>
#include <string>
#include <vector>

#include "support/source.hpp"

namespace phs {

enum class Severity : u8 { Error, Warning, Note };

struct DiagNote {
    Span span;
    std::string message;
};

struct Diagnostic {
    Severity severity;
    const char* code;  // e.g. "E0101"; stable across releases
    Span span;
    std::string message;
    std::vector<DiagNote> notes;
};

class DiagEngine {
public:
    explicit DiagEngine(const SourceManager& sm) : sm_(sm) {}

    Diagnostic& error(const char* code, Span span, std::string message);
    Diagnostic& warning(const char* code, Span span, std::string message);

    u32 error_count() const { return errors_; }
    const std::vector<Diagnostic>& all() const { return diags_; }

    void render(std::FILE* out) const;
    void render_one(std::FILE* out, const Diagnostic& d) const;

private:
    void render_snippet(std::FILE* out, Span span) const;

    const SourceManager& sm_;
    std::vector<Diagnostic> diags_;
    u32 errors_ = 0;
};

}  // namespace phs
