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

#include "support/source.hpp"

#include <algorithm>
#include <cstdio>

namespace phs {

bool SourceManager::load(const std::string& path, FileId& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::string text;
    char buf[64 * 1024];
    usize n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    bool ok = !std::ferror(f);
    std::fclose(f);
    if (!ok) return false;
    out = add(path, std::move(text));
    return true;
}

FileId SourceManager::add(std::string path, std::string text) {
    // Normalize line endings (CRLF and lone CR become LF) and drop a UTF-8 BOM
    // so every later stage sees a single canonical form.
    std::string norm;
    norm.reserve(text.size());
    usize i = 0;
    if (text.size() >= 3 && (u8)text[0] == 0xEF && (u8)text[1] == 0xBB && (u8)text[2] == 0xBF) i = 3;
    for (; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\r') {
            norm.push_back('\n');
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else {
            norm.push_back(c);
        }
    }
    SourceFile sf;
    sf.path = std::move(path);
    sf.text = std::move(norm);
    sf.line_starts.push_back(0);
    for (u32 k = 0; k < sf.text.size(); ++k)
        if (sf.text[k] == '\n') sf.line_starts.push_back(k + 1);
    files_.push_back(std::move(sf));
    return static_cast<FileId>(files_.size() - 1);
}

LineCol SourceManager::line_col(FileId id, u32 offset) const {
    const SourceFile& f = files_[id];
    auto it = std::upper_bound(f.line_starts.begin(), f.line_starts.end(), offset);
    u32 line = static_cast<u32>(it - f.line_starts.begin());
    u32 start = f.line_starts[line - 1];
    u32 col = 1;
    for (u32 k = start; k < offset && k < f.text.size(); ++k)
        if (((u8)f.text[k] & 0xC0) != 0x80) ++col;
    return {line, col};
}

std::string_view SourceManager::line_text(FileId id, u32 line) const {
    const SourceFile& f = files_[id];
    if (line == 0 || line > f.line_starts.size()) return {};
    u32 start = f.line_starts[line - 1];
    u32 end = line < f.line_starts.size() ? f.line_starts[line] - 1 : static_cast<u32>(f.text.size());
    return std::string_view(f.text).substr(start, end - start);
}

}  // namespace phs
