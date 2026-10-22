# Phantasos

Phantasos is an object-oriented programming language that compiles to native
code. It combines:

- **OOP that is safe by default**: immutable by default, deterministic
  construction and destruction (`init`/`deinit`), single inheritance with
  classes `final` by default, interfaces, sealed hierarchies, and no `null`.
- **Memory safety without a garbage collector**: single ownership with moves,
  opt-in `shared` (ARC) and `weak` references, and compile-time checked
  borrows that need no lifetime annotations.
- **Typed errors**: `throws(E)` is checked at compile time and compiled as
  zero-cost return values, with no stack unwinding.
- **Concurrency safety**: `async`/`await`, actors, and `Sendable` checking.
- **A toolchain written from scratch**: our own x86 backend, our own PE/ELF
  linker, and our own C11 frontend. It uses no LLVM and no libc
  (glibc/musl/MSVC CRT).

Supported targets: **Windows x64**, **Linux x86_64**, **Linux i386**.

## Status

The project is in **bootstrap phase P1**. The C++23 stage0 compiler in
[`bootstrap/stage0`](bootstrap/stage0) has these parts done and tested on
Windows x64 and Linux x86_64:

- [x] Lexer: significant newlines, nested comments, string interpolation,
  and full literal support
- [x] Parser for Phantasos-Core, which builds an AST with error recovery and
  stable diagnostic codes
- [ ] Semantic analysis (name resolution, types, ownership)
- [ ] MIR, x86_64 codegen, COFF/ELF writers, linker, runtime
- [ ] Workspace-aware build driver

The language is specified in [`docs/spec`](docs/spec).

## Repository layout

| Path | Purpose |
|---|---|
| `docs/spec/` | Language and ABI specification |
| `docs/adr/` | Architecture decision records |
| `bootstrap/stage0/` | Minimal C++23 bootstrap compiler with a workspace-aware build driver |
| `compiler/` | Self-hosted compiler (a Phantasos workspace), planned |
| `runtime/` | `rt`, the privileged runtime, planned |
| `std/` | Standard library (privileged), planned |
| `libc-compat/` | Optional C standard library for C sources, planned |
| `platform/` | Our own C declarations of OS interfaces |
| `tools/` | Developer tools (`license-check`, ...) |
| `tests/` | Conformance and golden tests |

## Building stage0

Requirements: clang++ with C++23 support, CMake 3.28+, and Ninja.

```sh
cmake -S bootstrap/stage0 -B build/stage0 -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug
cmake --build build/stage0
python tests/run_golden.py --stage0 build/stage0/phantasos-stage0      # golden tests
python tests/fuzz_frontend.py --stage0 build/stage0/phantasos-stage0   # crash/hang fuzzing
python tools/license-check/license_check.py                            # license headers
```

Try it:

```sh
build/stage0/phantasos-stage0 parse tests/stage0/parser/showcase.phs   # print the syntax tree
build/stage0/phantasos-stage0 lex   tests/stage0/lexer/literals.phs    # print the token stream
```

## License

Apache License 2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE). Every source
file must start with the project license header, and CI enforces this with
`tools/license-check`.
