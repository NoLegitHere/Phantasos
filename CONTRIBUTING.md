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
- No exceptions (`-fno-exceptions`) and no RTTI (`-fno-rtti`). Use `std::expected`
  and diagnostics instead.
- Keep use of the C++ standard library minimal, because stage0 is frozen after
  self-hosting.

## Tests

- Golden tests live in `tests/`. Each `*.phs` input has expected output files
  next to it (`*.tokens`, `*.ast`, `*.diag`).
- Regenerate them with `python tests/run_golden.py --stage0 <exe> --update`
  and review the diff before committing.
