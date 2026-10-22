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
#include <string_view>
#include <vector>

#include "support/common.hpp"

namespace phs {

using FileId = u32;

struct Span {
    FileId file = 0;
    u32 lo = 0;
    u32 hi = 0;

    static Span join(Span a, Span b) { return {a.file, a.lo, b.hi > a.hi ? b.hi : a.hi}; }
};

struct LineCol {
    u32 line;  // 1-based
    u32 col;   // 1-based, counted in Unicode scalar values
};

struct SourceFile {
    std::string path;
    std::string text;
    std::vector<u32> line_starts;
};

class SourceManager {
public:
    // Returns false if the file cannot be read.
    bool load(const std::string& path, FileId& out);
    FileId add(std::string path, std::string text);

    const SourceFile& file(FileId id) const { return files_[id]; }
    LineCol line_col(FileId id, u32 offset) const;
    std::string_view line_text(FileId id, u32 line) const;

private:
    std::vector<SourceFile> files_;
};

}  // namespace phs
