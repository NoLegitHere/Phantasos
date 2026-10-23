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
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "support/source.hpp"

namespace phs {

enum class Severity : u8 { Error, Warning, Note };

// Names are internal; the rendered codes remain stable across releases.
enum class DiagCode : u16 {
    UnexpectedCharacter = 1,
    UnterminatedComment = 2,
    UnterminatedString = 3,
    InvalidEscape = 4,
    NumberOutOfRange = 5,
    InvalidNumber = 6,
    InvalidCharacterLiteral = 7,
    NonAsciiCharacter = 8,
    UnexpectedClosingDelimiter = 9,
    UnclosedDelimiter = 10,
    InvalidUtf8 = 11,
    ExpectedToken = 100,
    ExpectedExpression = 101,
    ExpectedType = 102,
    ExpectedPattern = 103,
    ExpectedDeclaration = 104,
    ChainedComparison = 105,
    ExpectedTerminator = 106,
    ChainedRange = 107,
    LocalDeclaration = 108,
    ExternDeclaration = 109,
    ImportOrder = 110,
    DuplicateModifier = 111,
    NestingLimit = 112,
};

constexpr std::string_view diag_code(DiagCode code) {
    switch (code) {
        case DiagCode::UnexpectedCharacter: return "E0001";
        case DiagCode::UnterminatedComment: return "E0002";
        case DiagCode::UnterminatedString: return "E0003";
        case DiagCode::InvalidEscape: return "E0004";
        case DiagCode::NumberOutOfRange: return "E0005";
        case DiagCode::InvalidNumber: return "E0006";
        case DiagCode::InvalidCharacterLiteral: return "E0007";
        case DiagCode::NonAsciiCharacter: return "E0008";
        case DiagCode::UnexpectedClosingDelimiter: return "E0009";
        case DiagCode::UnclosedDelimiter: return "E0010";
        case DiagCode::InvalidUtf8: return "E0011";
        case DiagCode::ExpectedToken: return "E0100";
        case DiagCode::ExpectedExpression: return "E0101";
        case DiagCode::ExpectedType: return "E0102";
        case DiagCode::ExpectedPattern: return "E0103";
        case DiagCode::ExpectedDeclaration: return "E0104";
        case DiagCode::ChainedComparison: return "E0105";
        case DiagCode::ExpectedTerminator: return "E0106";
        case DiagCode::ChainedRange: return "E0107";
        case DiagCode::LocalDeclaration: return "E0108";
        case DiagCode::ExternDeclaration: return "E0109";
        case DiagCode::ImportOrder: return "E0110";
        case DiagCode::DuplicateModifier: return "E0111";
        case DiagCode::NestingLimit: return "E0112";
    }
    unreachable();
}

struct DiagNote {
    Span span;
    std::string message;
};

struct Diagnostic {
    Severity severity;
    DiagCode code;
    Span span;
    std::string message;
    std::vector<DiagNote> notes;
};

class DiagEngine {
public:
    explicit DiagEngine(const SourceManager& sm) : sm_(sm) {}

    // Returned references and all() views are invalidated by the next emission.
    template <typename... Args>
    Diagnostic& error(DiagCode code, Span span, std::format_string<Args...> fmt, Args&&... args) {
        ++errors_;
        return diags_.emplace_back(Severity::Error, code, span, std::format(fmt, std::forward<Args>(args)...));
    }

    template <typename... Args>
    Diagnostic& warning(DiagCode code, Span span, std::format_string<Args...> fmt, Args&&... args) {
        return diags_.emplace_back(Severity::Warning, code, span, std::format(fmt, std::forward<Args>(args)...));
    }

    usize error_count() const { return errors_; }
    std::span<const Diagnostic> all() const { return diags_; }

    void render(std::FILE* out) const;
    void render_one(std::FILE* out, const Diagnostic& d) const;

private:
    void render_snippet(std::FILE* out, Span span) const;

    const SourceManager& sm_;
    std::vector<Diagnostic> diags_;
    usize errors_ = 0;
};

}  // namespace phs
