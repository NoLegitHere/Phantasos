# Bootstrap roadmap

The roadmap follows the bootstrap strategy in
[ADR 0002](adr/0002-foundational-decisions.md). Checked entries describe
implemented behavior; an entire phase is complete only when its exit
criterion is met.

## P0: language and toolchain foundations

- [x] Draft language, package, concurrency and ABI specifications.
- [x] C++23 stage0 build on Windows x64 and Linux x86_64.
- [x] Source management, arena storage, interning and stable diagnostics.
- [x] License enforcement and CI.

Exit criterion: a documented Core language and a tested bootstrap toolchain
foundation. Complete.

## P1: stage0 native compiler

- [x] Lexer, parser, error recovery, syntax dumps and frontend library.
- [x] Standalone [Core EBNF grammar](spec/phantasos.ebnf).
- [x] Initial semantic validation: duplicate bindings, declaration contexts,
  type alias forms, pattern binding consistency and control-flow placement.
- [x] File-local name resolution: collect declarations and lexical scopes,
  resolve value/type references, and retain bindings and overload sets.
  Cover forward declarations, generic scopes and sequential local shadowing.
- [ ] **Next: type checking and inference.** Built-ins, calls, operators, conversions,
  generics, optionals, inheritance, visibility, and typed errors.
- [ ] Definite initialization, moves, borrows and destructor checks.
- [ ] Typed intermediate representation and lowering.
- [ ] x86_64 code generation and COFF/ELF object writers.
- [ ] Native PE/ELF linker, minimal runtime and entry points.
- [ ] Native hello-world executables on Windows x64 and Linux x86_64.

`check` performs syntax, structural semantic validation and file-local lexical
name resolution. Member lookup that needs receiver types, alias expansion or
inheritance is retained as deferred work for type checking. A successful check
does **not** yet imply that all members exist, types match, ownership is valid,
or executable code can be generated. Imports receive an explicit unsupported
diagnostic until P2 package loading exists. `parse` remains syntax-only.

Exit criterion: stage0 can compile, link and run Core programs natively on
both initial targets. In progress.

## P2: package and workspace support

- [ ] Parse the documented `Phantasos.toml` subset.
- [ ] Discover source modules and workspace members.
- [ ] Resolve local dependencies, report cycles and build in dependency order.
- [ ] Produce and consume `.phsi` library interfaces and object archives.
- [ ] Link libraries into binary packages.

Exit criterion: stage0 can build the multi-package compiler workspace from
manifests. See [the package specification](spec/04-packages-and-modules.md)
for the deliberately limited stage0 build-driver scope.

## Self-hosting and full-language work

- [ ] Port the compiler to Phantasos and build stage1 with stage0.
- [ ] Produce byte-identical stage2 and stage3 binaries.
- [ ] Freeze the bootstrap compiler after reproducible self-hosting.
- [ ] Extend the self-hosted toolchain with the remaining targets, runtime,
  standard library, C frontend, and full-language features.

Incremental caching, lockfiles, git dependencies, parallel builds and
cross-compilation belong to the self-hosted build tool, not stage0.
