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
#include <unordered_map>
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
    DuplicateBinding = 200,
    InvalidDeclarationContext = 201,
    InvalidTypeAlias = 202,
    InvalidControlFlow = 203,
    ThrowOutsideThrows = 204,
    AwaitOutsideAsync = 205,
    PatternBindingMismatch = 206,
    InvalidParameterBinding = 207,
    UnknownValue = 300,
    UnknownType = 301,
    ConflictingDeclaration = 302,
    InvalidReceiver = 303,
    ImportUnavailable = 304,
    ScalarTypeMismatch = 400,
    InvalidScalarOperand = 401,
    ScalarLiteralRange = 402,
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
        case DiagCode::DuplicateBinding: return "E0200";
        case DiagCode::InvalidDeclarationContext: return "E0201";
        case DiagCode::InvalidTypeAlias: return "E0202";
        case DiagCode::InvalidControlFlow: return "E0203";
        case DiagCode::ThrowOutsideThrows: return "E0204";
        case DiagCode::AwaitOutsideAsync: return "E0205";
        case DiagCode::PatternBindingMismatch: return "E0206";
        case DiagCode::InvalidParameterBinding: return "E0207";
        case DiagCode::UnknownValue: return "E0300";
        case DiagCode::UnknownType: return "E0301";
        case DiagCode::ConflictingDeclaration: return "E0302";
        case DiagCode::InvalidReceiver: return "E0303";
        case DiagCode::ImportUnavailable: return "E0304";
        case DiagCode::ScalarTypeMismatch: return "E0400";
        case DiagCode::InvalidScalarOperand: return "E0401";
        case DiagCode::ScalarLiteralRange: return "E0402";
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

    // Returned references, all() views and in_source_order() pointers are
    // invalidated by the next emission.
    template <typename... Args>
    Diagnostic& error(DiagCode code, Span span, std::format_string<Args...> fmt, Args&&... args) {
        ++errors_;
        ++errors_by_file_[span.file.value];
        return diags_.emplace_back(Severity::Error, code, span, std::format(fmt, std::forward<Args>(args)...));
    }

    template <typename... Args>
    Diagnostic& warning(DiagCode code, Span span, std::format_string<Args...> fmt, Args&&... args) {
        return diags_.emplace_back(Severity::Warning, code, span, std::format(fmt, std::forward<Args>(args)...));
    }

    usize error_count() const { return errors_; }
    usize error_count(FileId file) const {
        const auto it = errors_by_file_.find(file.value);
        return it == errors_by_file_.end() ? 0 : it->second;
    }
    bool has_errors(FileId file) const { return error_count(file) != 0; }
    // Emission order, which can differ from source order across compiler passes.
    std::span<const Diagnostic> all() const { return diags_; }
    // A separate view ordered by FileId and byte offset; ties keep emission
    // order. Does not reorder storage. The caller owns the returned vector.
    [[nodiscard]] std::vector<const Diagnostic*> in_source_order() const;

    void render(std::FILE* out) const;
    void render_one(std::FILE* out, const Diagnostic& d) const;

private:
    void render_snippet(std::FILE* out, Span span) const;

    const SourceManager& sm_;
    std::vector<Diagnostic> diags_;
    usize errors_ = 0;
    std::unordered_map<u32, usize> errors_by_file_;
};

}  // namespace phs
