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
  `main.cpp`. Prefer `FrontendSession` (`frontend/session.hpp`) to own sources,
  keyword-seeded interning, diagnostics, AST storage and cached resolutions.
  `parse(FileId)` is syntax-only; `check(FileId)` runs validation, resolution
  and scalar typing only if that file has no errors. Repeated calls reuse results without
  emitting diagnostics again. Files are immutable and resolve independently;
  source IDs and result pointers belong to the session, which cannot be moved
  or copied. A non-null `CheckResult::resolution` means resolution ran, not
  that it succeeded: consult `diagnostics().has_errors(file)`.
  `CheckResult::scalar_types` is non-null when resolution succeeded and scalar
  checking ran, even if type diagnostics were emitted. `ScalarTypes` retains
  expression and binding facts without modifying the AST; unsupported facts
  are `Deferred`, diagnosed failures are `Error`. Keep new type-dependent
  rules in `sema/types`, with explicit stacks for traversal and dependencies.
  Scalar signatures keep function return types separate from function value
  bindings. Call records distinguish selected, deferred and erroneous calls;
  only selected targets carry argument-to-parameter mappings. Keep speculative
  overload probes free of diagnostics and AST/type mutation; contextualize
  literals only after selecting a target.
  `ScalarType` is a value containing a built-in kind and optional depth. Keep
  optional identity through bindings, signatures and call matching; never
  classify an optional numeric value as a numeric operand. `CallKind` separates
  function targets from intrinsic numeric type targets. Conversion calls finish
  immediately and must not create function-return dependencies. Conversion
  records retain modes, source/destination facts, and deferred/error states.
  Alias expansion runs before scalar annotations and retains canonical binding
  IDs rather than creating new types. Concrete generic applications substitute
  aliases and signatures in interned environments keyed by declaration identity
  and arguments; keep per-environment caches separate from declaration facts.
  Inference uses internal type-parameter descriptors and supplied argument facts;
  concrete arguments anchor numeric literals before defaulting. Keep declaration
  signatures unchanged and speculative probes free of literal mutation. Concrete
  interface/where constraints require compatible supported implementations, not
  ancestry alone; retain constraint states separately from structural identities.
  Generic body validation and dependent/associated-type contracts remain deferred.
  Keep alias/type wrapper traversal
  iterative, report cycle paths once, and do not mutate lexical resolution.
  Type queries cache diagnostics so shared constructor annotations are not
  diagnosed once for each generated parameter/field binding.
  Composite/nominal facts carry result-local descriptor IDs; intern structural
  keys so aliases share identity. Descriptor pointers must not survive interning
  during a check. Completed result pointers remain stable. Type normalization,
  formatting, array materialization and overload probes use explicit work stacks.
  Keep type-name formatting bounded in both work and output; shared descriptor
  graphs can expand exponentially even with iterative traversal. Public names
  default to 32 KiB and diagnostic names to 1 KiB per type, with `...` elision.
  Use the checked nominal graph for ancestry; generic/built-in trait contracts
  remain deferred. Primary constructors and typed indirect calls finish without
  adding function-return dependencies; named-function value inference is pending.
  Member lookup retains separate `CheckedMember` records; never rewrite lexical
  references. Index direct member scopes and cache expression type entities.
  Resolve a receiver before adding its selected field dependency. Method calls
  add only the selected return dependency, as direct functions do. Inherited
  lookup shares cached results along chains and deduplicates diamonds. Normalize
  newly available inherited type/base aliases before checking callable signatures.
  Keep structural graph state separate from conformance/witness state. Unsupported
  generic, associated-type, property and receiver-convention contracts are pending.
  For low-level API calls, seed the interner with `seed_keywords` before lexing. Source
  managers, interners, and arenas must outlive the views/nodes referring to
  their storage. `DiagEngine::all()` exposes emission order;
  `in_source_order()` returns a separate view sorted by file and offset,
  preserving emission order at ties. Diagnostic references, `all()` spans and
  pointers in the sorted view must not be retained across subsequent emissions.
  `error_count(FileId)` and `has_errors(FileId)` track error emissions by their
  primary file; warnings and notes pointing into other files do not count.
  The no-argument `error_count()` remains the total for the whole engine.
- After a file parses without lexer/parser errors, `validate_file` performs
  initial semantic validation without changing the AST. Keep type-dependent
  checks in future resolution/type passes. Use explicit work stacks for
  traversing ASTs: flat expression chains can be much deeper than parser
  nesting limits. Add semantic rules to `docs/spec` and update the
  [roadmap](docs/roadmap.md) when a milestone advances.
- After structural validation succeeds, `resolve_file` returns an owning
  `Resolution` containing scopes, declaration IDs, shared overload sets and
  reference records. Keep its AST and interner alive. IDs belong to one
  result; views remain valid until that result is moved or destroyed.
  Use `reference(Span)` and `candidates(target)` to inspect a use. A deferred
  reference requires later type or package information; it is not a successful
  member lookup. Scope binding lists describe the completed scope; reference
  targets preserve sequential visibility at each use. Constructor defaults
  expose the same parameter IDs as the primary initializer.
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
  The `resolve` suite checks binding identity, forward declarations, overload
  sets, namespaces, generics, sequential visibility, constructor scopes,
  pattern scopes, deferred references, file isolation and iterative traversal.
  The `session` suite checks phase gating, per-file error counts, warning/note
  handling, cached calls, source limits and result lifetimes across file growth.
  The `types` suite checks contextual scalar inference, forward dependencies,
  numeric limits, operator domains, defaults, returns, conditions, assignments,
  deferred facts, shadowing, AST immutability, session lifetimes and long trees.
  The `calls` suite covers signature/return inference, forward and recursive
  calls, positional/named arguments, defaults, overload and range selection,
  deferred candidates, diagnostics, stable results and long call chains.
  The `conversions` suite covers widening boundaries, checked/truncating integer
  APIs, optional result identity, promotion, arguments and operators, name
  shadowing, deferred policies, cached facts and long forward dependencies.
  The `aliases` suite checks forward chains, cycles through wrappers and
  qualified names, declaration-scope capture, namespace shadowing, canonical
  nominal identity, conversion targets, diagnostic caching, file isolation,
  stable records, and 10,000 aliases with accumulated optional depth.
  The `composites` suite covers canonical descriptors, contextual/parenthesized
  arrays, indexing, nominal identity/constructors, pointer/function signatures,
  typed indirect calls, deferred relations, stable records and deep type graphs.
  The `members` suite covers direct fields/methods, static and implicit receivers,
  alias-qualified access, member overloads/defaults, callable fields, receiver
  errors, deferred hierarchies/generics, stable records and 10,000-step chains.
  The `hierarchy` suite covers bases/cycles, nominal/interface upcasts, inherited
  fields/types/overloads, outer-scope fallback, `super`, overrides and witnesses,
  defaults/abstract obligations, forward base aliases, invalid/deferred contracts,
  stable records, 10,000-class chains and 64-level interface diamonds.
  The `generics` suite checks explicit type/function applications, signature and
  member substitution, shadowed parameters, invariant identities, argument errors,
  deferred contracts/defaults, stable records, 10,000 aliases and shared type graphs.
  The `inference` suite covers literal/concrete anchors, composite patterns,
  constructors/methods, overload filtering, concrete bounds/where clauses,
  implementation-return dependencies, retained errors/deferrals, shared patterns
  and 5,000-interface chains.

- Run `python tests/test_license_check.py` for license-fixer regressions,
  including encoding declarations, BOMs, line endings, and idempotence.
- Run `python tests/test_golden_runner.py` for golden-runner exit status and
  baseline-update regressions.
- Golden tests live in `tests/`. Each `*.phs` input has expected output files
  next to it (`*.tokens`, `*.ast`, `*.diag`).
- `tests/stage0/diagnostics/` contains negative `check` cases and always
  expects exit code 1, including during `--update`.
- `tests/stage0/semantics/` runs `check`; an error diagnostic header in the
  expected `.diag` file requires exit code 1. Empty and warning-only files
  require success; source snippets and notes are not error headers. `parse`
  fixtures exercise syntax independently of semantic validity.
- Regenerate them with `python tests/run_golden.py --stage0 <exe> --update`
  and review the diff before committing.
  Lexer/parser fixtures are positive unless an existing `.diag` baseline has
  a primary error header. Updates must preserve that expected exit status
  before writing any files. Intentionally negative lexer/parser cases need
  that diagnostic baseline first. A filter matching no cases is an error.
- Run `python tests/test_frontend.py --stage0 <exe>` for deterministic nesting,
  long AST/token traversal, and parser-state regression tests. These run on both
  platforms and in the sanitizer job, alongside the mutation fuzzer.
- Run the mutation fuzzer with `--command check` as well as its default
  `parse` mode to exercise semantic validation after successful parses.
