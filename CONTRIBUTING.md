# Contributing to Phantasos

## Ground rules

1. **License.** All contributions are made under the Apache License 2.0.
2. **The license header is mandatory.** Every source file must begin with the
   project header, using that file's comment syntax:

   ```
   // Copyright <YEAR> The Phantasos Authors
   // SPDX-License-Identifier: Apache-2.0
   //
   // Licensed under the Apache License, Version 2.0 (the "License");
   // ...
   ```

   Run `python tools/license-check/license_check.py --fix` to insert any
   missing headers. CI fails if a header is missing.
3. **Write it from scratch.** Do not copy code or headers from LLVM, glibc,
   musl, the MSVC runtime, the Windows SDK, or the Linux kernel. Platform
   declarations in `platform/` must be written by us from public
   documentation (only facts such as syscall numbers, struct layouts, and
   function signatures).
4. **Developer Certificate of Origin.** Sign off every commit
   (`git commit -s`), which certifies the DCO (https://developercertificate.org).

## Stage0 C++ rules

- C++23, compiled with clang++ on both Windows and Linux.
- Use a compiler/standard-library combination that passes the CMake feature
  probe. Linux CI selects Clang 19 and libstdc++ 14 explicitly; see the README.
- No exceptions (`-fno-exceptions`) and no RTTI (`-fno-rtti`). Use `std::expected`
  and diagnostics instead.
- Use `FileId`, `DiagCode`, and `ast::Modifier` instead of raw integers or
  diagnostic-code strings. Keep rendered diagnostic codes stable. Use
  `ast::as<T>` for a nullable kind check and `ast::cast<T>` for an asserted
  downcast; both preserve source-pointer constness. Type declarations have
  `as_type_decl` and `cast_type_decl` because several kinds share one node.
- Arena objects must be trivially destructible and constructible from the
  supplied arguments. `Arena::copy` requires a sized, contiguous range that
  can be read through a const reference and whose elements can be copied.
  These constraints follow the [C++ Core Guidelines on constrained templates](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#t10-specify-concepts-for-all-template-arguments).
- The CLI and C++ tests link `Phantasos::Frontend`; keep CLI-only behavior in
  `main.cpp`. Seed the interner with `seed_keywords` before lexing. Source
  managers, interners, and arenas must outlive the views/nodes referring to
  their storage. Diagnostic references and `all()` spans must not be retained
  across subsequent diagnostic emissions.
- After a file parses without lexer/parser errors, `validate_file` performs
  initial semantic validation without changing the AST. Keep type-dependent
  checks in future resolution/type passes. Use explicit work stacks for
  traversing ASTs: flat expression chains can be much deeper than parser
  nesting limits. Add semantic rules to `docs/spec` and update the
  [roadmap](docs/roadmap.md) when a milestone advances.
- Keep use of the C++ standard library minimal, because stage0 is frozen after
  self-hosting.

## Tests

- `ctest --test-dir build/stage0 --output-on-failure` runs the focused C++
  suites in `bootstrap/stage0/tests/`, including compile-time API constraints,
  source I/O and limits, arena alignment/lifetime, Unicode diagnostics,
  multi-file isolation, and AST structure/recovery. These tests link the same
  frontend library as the CLI and run in both platform jobs and under sanitizers.
  The `sema` suite covers declaration and control-flow contexts, binding
  diagnostics and notes, pattern alternatives, AST immutability, and long trees.

- Run `python tests/test_license_check.py` for license-fixer regressions,
  including encoding declarations, BOMs, line endings, and idempotence.
- Golden tests live in `tests/`. Each `*.phs` input has expected output files
  next to it (`*.tokens`, `*.ast`, `*.diag`).
- `tests/stage0/semantics/` runs `check`; a nonempty error diagnostic file
  expects exit code 1, and an empty `.diag` file expects success. `parse`
  fixtures continue to exercise syntax independently of semantic validity.
- Regenerate them with `python tests/run_golden.py --stage0 <exe> --update`
  and review the diff before committing.
- Run `python tests/test_frontend.py --stage0 <exe>` for deterministic nesting,
  long AST/token traversal, and parser-state regression tests. These run on both
  platforms and in the sanitizer job, alongside the mutation fuzzer.
- Run the mutation fuzzer with `--command check` as well as its default
  `parse` mode to exercise semantic validation after successful parses.
