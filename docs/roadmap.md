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
- [x] Frontend session ownership, cached phase results and per-file diagnostics.
- [x] Initial scalar typing: retained expression/binding types, built-in annotations,
  contextual numeric literals and ranges, forward variable inference, scalar
  operators, Boolean conditions, defaults, explicit returns and assignments.
- [x] Initial callable signatures and direct file-level calls: scalar expression-body
  return inference, arguments, labels, defaults, overload selection and retained targets.
- [x] Initial numeric conversions: lossless integer and floating widening,
  checked/truncating integers, diagnostics, retained conversion records and
  optional wrappers on built-in type facts.
- [x] Initial alias expansion: non-generic chains, cycles, optional wrappers,
  canonical built-in/nominal targets, direct qualified types and numeric aliases.
- [x] Initial composite/nominal checking: canonical arrays, pointers, ownership
  wrappers, function types and non-generic nominal instances; array literals and
  indexing, exact type matching, primary constructors and typed indirect calls.
- [x] Initial receiver-dependent members: direct fields and methods on known
  non-generic nominal receivers, static/instance access, aliases and retained targets.
- [x] Initial inheritance/conformance: non-generic base graphs, cycles, inherited
  member/type/overload lookup, overrides, method requirements and nominal upcasts.
- [ ] **Next: generic type checking.** Instantiation/substitution, `where`
  constraints, associated types and dependent conformance contracts.
- [ ] Complete type checking: generic aliases, generics, optionals,
  generic calls, function-value inference, remaining numeric policies, receiver-dependent members,
  inheritance, visibility, and typed errors.
- [ ] Definite initialization, moves, borrows and destructor checks.
- [ ] Typed intermediate representation and lowering.
- [ ] x86_64 code generation and COFF/ELF object writers.
- [ ] Native PE/ELF linker, minimal runtime and entry points.
- [ ] Native hello-world executables on Windows x64 and Linux x86_64.

`check` performs syntax, structural semantic validation, file-local lexical
name resolution, initial scalar typing and direct scalar function calls.
Unsupported type facts, including generic calls, function-value inference, remaining numeric policies and
block/branch results, are explicitly deferred. Direct nominal field and method
lookup runs after receiver inference, including known inherited members and
type aliases. Supported method requirements are checked with retained witnesses.
Generic substitution, dependent/property contracts, visibility,
optional/ownership-wrapper members and bound method values remain pending. A successful
check does **not** yet imply that all members exist, types match, ownership is valid,
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
