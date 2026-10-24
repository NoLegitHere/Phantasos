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
#include <print>
#include <span>
#include <string_view>
#include <utility>

#include "sema/resolve.hpp"
#include "sema/validate.hpp"
#include "support/arena.hpp"
#include "support/diag.hpp"
#include "support/interner.hpp"
#include "support/source.hpp"
#include "syntax/ast_dump.hpp"
#include "syntax/lexer.hpp"
#include "syntax/parser.hpp"
#include "syntax/token.hpp"

namespace {

constexpr std::string_view kUsage =
    "usage: phantasos-stage0 <command> [args]\n"
    "\n"
    "commands:\n"
    "  lex <file.phs>     print the token stream\n"
    "  parse <file.phs>   print the syntax tree\n"
    "  check <file.phs>   check syntax, semantics and lexical names\n"
    "  version            print the version\n"
    "  help               print this message\n";

int run_frontend(std::string_view cmd, std::string_view path) {
    phs::SourceManager sm;
    const auto file = sm.load(path);
    if (!file) {
        std::println(stderr, "error: cannot read '{}': {}", path, file.error().message());
        return 2;
    }
    phs::Interner interner;
    phs::seed_keywords(interner);
    phs::DiagEngine diag(sm);

    std::vector<phs::Token> tokens = phs::lex_file(sm, *file, interner, diag);
    if (cmd == "lex") {
        std::print("{}", phs::dump_tokens(tokens, interner, sm));
    } else {
        phs::Arena arena;
        const phs::ast::File* ast = phs::parse_file(*file, std::move(tokens), arena, interner, diag);
        if (cmd == "parse") std::print("{}", phs::dump_ast(*ast, interner));
        else if (diag.error_count() == 0) {
            phs::validate_file(*ast, interner, diag);
            if (diag.error_count() == 0) {
                [[maybe_unused]] const auto resolution = phs::resolve_file(*ast, interner, diag);
            }
        }
    }
    std::fflush(stdout);
    diag.render(stderr);
    return diag.error_count() ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::span<char*> args(argv, static_cast<std::size_t>(argc));
    if (args.size() < 2) {
        std::print(stderr, "{}", kUsage);
        return 2;
    }
    const std::string_view cmd = args[1];
    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        std::print("{}", kUsage);
        return 0;
    }
    if (cmd == "version" || cmd == "--version") {
        std::println("phantasos-stage0 {}", PHS_VERSION);
        return 0;
    }
    if (cmd == "lex" || cmd == "parse" || cmd == "check") {
        if (args.size() != 3) {
            std::print(stderr, "{}", kUsage);
            return 2;
        }
        return run_frontend(cmd, args[2]);
    }
    std::print(stderr, "error: unknown command '{}'\n\n{}", cmd, kUsage);
    return 2;
}
