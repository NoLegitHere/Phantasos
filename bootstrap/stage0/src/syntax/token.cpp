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

#include "syntax/token.hpp"

namespace phs {

namespace {

const char* const kNames[] = {
#define PHS_X(name, spelling) #name,
    PHS_TOKENS(PHS_X)
#undef PHS_X
#define PHS_K(name, text) "Kw" #name,
    PHS_KEYWORDS(PHS_K)
#undef PHS_K
};

const char* const kSpellings[] = {
#define PHS_X(name, spelling) spelling,
    PHS_TOKENS(PHS_X)
#undef PHS_X
#define PHS_K(name, text) "'" text "'",
    PHS_KEYWORDS(PHS_K)
#undef PHS_K
};

const char* const kKeywordTexts[] = {
#define PHS_K(name, text) text,
    PHS_KEYWORDS(PHS_K)
#undef PHS_K
};

constexpr u32 kKeywordCount = sizeof(kKeywordTexts) / sizeof(kKeywordTexts[0]);
constexpr u32 kFirstKeyword = static_cast<u32>(Tok::KwAbstract);

static_assert(sizeof(kNames) / sizeof(kNames[0]) == static_cast<usize>(Tok::Count_));

}  // namespace

const char* tok_name(Tok k) { return kNames[static_cast<usize>(k)]; }
const char* tok_spelling(Tok k) { return kSpellings[static_cast<usize>(k)]; }
bool tok_is_keyword(Tok k) { return static_cast<u32>(k) >= kFirstKeyword && k != Tok::Count_; }

void seed_keywords(Interner& in) {
    PHS_ASSERT(in.size() == 1);
    for (u32 i = 0; i < kKeywordCount; ++i) {
        Symbol s = in.intern(kKeywordTexts[i]);
        PHS_ASSERT(s.id == i + 1);
    }
}

Tok keyword_kind(Symbol s) {
    if (s.id >= 1 && s.id <= kKeywordCount) return static_cast<Tok>(kFirstKeyword + s.id - 1);
    return Tok::Ident;
}

}  // namespace phs
