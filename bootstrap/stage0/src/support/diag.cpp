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

#include "support/diag.hpp"

#include <algorithm>

#include "support/utf8.hpp"

namespace phs {

Diagnostic& DiagEngine::error(const char* code, Span span, std::string message) {
    ++errors_;
    diags_.push_back({Severity::Error, code, span, std::move(message), {}});
    return diags_.back();
}

Diagnostic& DiagEngine::warning(const char* code, Span span, std::string message) {
    diags_.push_back({Severity::Warning, code, span, std::move(message), {}});
    return diags_.back();
}

void DiagEngine::render(std::FILE* out) const {
    // Present diagnostics in source order regardless of which phase found them.
    std::vector<const Diagnostic*> sorted;
    sorted.reserve(diags_.size());
    for (const Diagnostic& d : diags_) sorted.push_back(&d);
    std::stable_sort(sorted.begin(), sorted.end(), [](const Diagnostic* a, const Diagnostic* b) {
        if (a->span.file != b->span.file) return a->span.file < b->span.file;
        return a->span.lo < b->span.lo;
    });
    for (const Diagnostic* d : sorted) render_one(out, *d);
}

void DiagEngine::render_one(std::FILE* out, const Diagnostic& d) const {
    const char* sev = d.severity == Severity::Error ? "error" : d.severity == Severity::Warning ? "warning" : "note";
    LineCol lc = sm_.line_col(d.span.file, d.span.lo);
    std::fprintf(out, "%s:%u:%u: %s[%s]: %s\n", sm_.file(d.span.file).path.c_str(), lc.line, lc.col, sev, d.code,
                 d.message.c_str());
    render_snippet(out, d.span);
    for (const DiagNote& n : d.notes) {
        LineCol nlc = sm_.line_col(n.span.file, n.span.lo);
        std::fprintf(out, "%s:%u:%u: note: %s\n", sm_.file(n.span.file).path.c_str(), nlc.line, nlc.col,
                     n.message.c_str());
        render_snippet(out, n.span);
    }
}

void DiagEngine::render_snippet(std::FILE* out, Span span) const {
    LineCol lc = sm_.line_col(span.file, span.lo);
    std::string_view line = sm_.line_text(span.file, lc.line);

    // Echo the line with invalid UTF-8 bytes replaced by U+FFFD so that the
    // output stays valid UTF-8; each replacement is one column, matching
    // utf8_step() used for caret placement and SourceManager::line_col().
    std::fprintf(out, "%5u | ", lc.line);
    for (usize k = 0; k < line.size();) {
        u32 cp;
        u32 len = utf8_decode(line.data() + k, line.size() - k, &cp);
        if (len) {
            std::fwrite(line.data() + k, 1, len, out);
            k += len;
        } else {
            std::fputs("\xEF\xBF\xBD", out);
            ++k;
        }
    }
    std::fputs("\n      | ", out);

    const std::string& text = sm_.file(span.file).text;
    u32 line_start = static_cast<u32>(line.data() - text.data());
    u32 line_end = line_start + static_cast<u32>(line.size());
    for (u32 k = line_start; k < span.lo && k < line_end; k += utf8_step(text.data() + k, line_end - k))
        std::fputc(text[k] == '\t' ? '\t' : ' ', out);
    u32 hi = span.hi < line_end ? span.hi : line_end;
    u32 width = 0;
    for (u32 k = span.lo; k < hi; k += utf8_step(text.data() + k, hi - k)) ++width;
    if (width == 0) width = 1;
    for (u32 k = 0; k < width; ++k) std::fputc('^', out);
    std::fputc('\n', out);
}

}  // namespace phs
