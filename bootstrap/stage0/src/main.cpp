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

// phantasos-stage0: the C++23 bootstrap compiler for Phantasos-Core.

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "support/arena.hpp"
#include "support/diag.hpp"
#include "support/interner.hpp"
#include "support/source.hpp"
#include "syntax/ast_dump.hpp"
#include "syntax/lexer.hpp"
#include "syntax/parser.hpp"
#include "syntax/token.hpp"

namespace {

void usage(std::FILE* out) {
    std::fputs(
        "usage: phantasos-stage0 <command> [args]\n"
        "\n"
        "commands:\n"
        "  lex <file.phs>     print the token stream\n"
        "  parse <file.phs>   print the syntax tree\n"
        "  check <file.phs>   report syntax errors only\n"
        "  version            print the version\n"
        "  help               print this message\n",
        out);
}

int run_frontend(std::string_view cmd, const char* path) {
    phs::SourceManager sm;
    phs::FileId file;
    if (!sm.load(path, file)) {
        std::fprintf(stderr, "error: cannot read '%s'\n", path);
        return 2;
    }
    phs::Interner interner;
    phs::seed_keywords(interner);
    phs::DiagEngine diag(sm);

    std::vector<phs::Token> tokens = phs::lex_file(sm, file, interner, diag);
    if (cmd == "lex") {
        std::fputs(phs::dump_tokens(tokens, interner, sm).c_str(), stdout);
    } else {
        phs::Arena arena;
        phs::ast::File* ast = phs::parse_file(file, std::move(tokens), arena, interner, diag);
        if (cmd == "parse") std::fputs(phs::dump_ast(*ast, interner).c_str(), stdout);
    }
    std::fflush(stdout);
    diag.render(stderr);
    return diag.error_count() ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    std::string_view cmd = argv[1];
    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        usage(stdout);
        return 0;
    }
    if (cmd == "version" || cmd == "--version") {
        std::printf("phantasos-stage0 %s\n", PHS_VERSION);
        return 0;
    }
    if (cmd == "lex" || cmd == "parse" || cmd == "check") {
        if (argc != 3) {
            usage(stderr);
            return 2;
        }
        return run_frontend(cmd, argv[2]);
    }
    std::fprintf(stderr, "error: unknown command '%.*s'\n\n", (int)cmd.size(), cmd.data());
    usage(stderr);
    return 2;
}
