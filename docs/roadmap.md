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
- [x] Initial generic instantiation/substitution: concrete nominal and transparent
  alias applications, explicit function/method arguments, substituted constructor
  and direct member signatures, and canonical invariant instance identities.
- [x] Initial generic argument inference and concrete constraints: supplied
  argument anchors, literal context, nominal constructors, interface bounds,
  `where` clauses and retained constraint states.
- [ ] **Next: generic bodies and dependent contracts.** Check generic bodies
  once against bounds, resolve members through bounds, and complete associated
  types, built-in traits, generic inheritance and dependent conformance.
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
Unsupported type facts, including dependent generic calls, function-value inference, remaining numeric policies and
block/branch results, are explicitly deferred. Direct nominal field and method
lookup runs after receiver inference, including known inherited members and
type aliases. Supported method requirements are checked with retained witnesses.
General inference/bound solving, captured owners, dependent/property contracts, visibility,
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

### Readiness gates before the port

The normative [Core subset](spec/README.md) is the language of the self-hosted
compiler. The following gates turn the remaining deferrals into a concrete
bootstrap plan; successful parsing or today's partial `check` is insufficient.

1. **Close Core semantic gaps.** Finish generic instantiation/substitution,
   generic aliases/calls, inference and `where` constraints, then dependent
   conformance (associated types, `Self`, properties and required built-in
   contracts). Complete optionals, casts, block/branch result types,
   function/bound-method values, constructor selection/delegation, visibility,
   numeric policies, typed errors, unsafe contexts and privileged intrinsics.
   Track implementation and tests against every Core construct; no unresolved
   required type, call, member or witness may enter executable lowering.
   Unsupported constructs must produce diagnostics instead of silently passing
   this gate.
2. **Enforce Core safety and cleanup.** Add all-path returns, exhaustiveness,
   definite initialization, mutable-place rules, moves, borrows and calling
   conventions, plus destruction on every control-flow exit. Implement the
   required `Copy`/`Error` contracts and `shared`/`weak` ownership behavior.
3. **Execute native Core programs.** Finish typed IR and lowering, layouts,
   virtual/witness dispatch, typed-error returns and cleanup. Complete the
   [ABI](spec/abi.md), including collision-free overload/type mangling, before
   object emission, and implement the required `extern "C"` ABI. Supply the
   x86_64 backends, COFF/ELF writers, PE/ELF linkers and the minimal runtime/library
   surface the compiler needs (allocation, collections, strings, files, diagnostics
   and process entry). Run compiler-sized Core workloads on Windows x64 and Linux
   x86_64, using the own-toolchain policy in
   [ADR 0002](adr/0002-foundational-decisions.md).
4. **Finish P2 before porting.** Build a multi-package workspace from manifests,
   resolve dependencies, emit/read `.phsi` interfaces and link libraries. A
   file-local checker cannot build the compiler workspace.
5. **Prove the replacement before freezing stage0.** Build stage1 with stage0,
   compare accepted/rejected programs and executable behavior across both
   compilers, then require byte-identical stage2/stage3 binaries with controlled
   inputs and the same target/build configuration. Keep stage0 available until
   these checks pass on both initial targets.

Full-language features outside Core, including concurrency if the compiler
does not use it, can follow self-hosting. Any change to the documented Core
boundary must be explicit and precede relying on a smaller bootstrap subset.
The remaining targets, full C11 frontend, and full runtime/standard library also
remain later work; implement their bootstrap-required surface first. Incremental
build features listed below are not prerequisites for the port.

### Transition and later work

- [ ] Port the compiler to Phantasos and build stage1 with stage0.
- [ ] Produce byte-identical stage2 and stage3 binaries.
- [ ] Freeze the bootstrap compiler after reproducible self-hosting.
- [ ] Extend the self-hosted toolchain with the remaining targets, runtime,
  standard library, C frontend, and full-language features.

Incremental caching, lockfiles, git dependencies, parallel builds and
cross-compilation belong to the self-hosted build tool, not stage0.
