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
- [x] Initial semantic validation: bindings, declarations and control flow
- [x] File-local lexical name resolution with retained bindings and overload sets
- [x] Initial scalar type checking, contextual literals and binding inference
- [x] Scalar function signatures, return inference and direct call/overload checking
- [x] Initial numeric conversions: widening, checked/truncating integers and optional result facts
- [x] Non-generic alias expansion, cycle diagnostics and canonical type identities
- [x] Initial composite/nominal identities, arrays, primary constructors and typed indirect calls
- [x] Direct nominal field/method lookup, static/instance access and member call targets
- [x] Initial inheritance graphs, inherited lookup, overrides, method conformance and nominal upcasts
- [x] Initial explicit generic applications and alias/signature/member substitution
- [x] Initial argument-based generic inference and concrete interface/where constraints
- [x] Initial generic body checking, bound-based lookup, associated types/Self and dependent witnesses
- [x] Flow checks: all-path returns, definite initialization, immutable bindings and fields, `mut fun` receivers and branch results
- [ ] Generic base substitution, built-in traits, remaining dependent contracts and ownership analysis
- [x] Native scalar slice: MIR, naive x86_64 code, and static Linux ELF executables with a stage0 runtime (`build`)
- [x] Output: string literals and a prelude `print` for `String`, `Int`, `UInt` and `Bool`
- [ ] Windows PE/COFF output, a general linker and runtime, and code generation beyond scalars
- [ ] Workspace-aware build driver

The language is specified in [`docs/spec`](docs/spec), with a standalone
[Core EBNF grammar](docs/spec/phantasos.ebnf).
See the [bootstrap roadmap](docs/roadmap.md) for milestones and the next steps.

`check <file.phs>` runs structural semantic validation, lexical name
resolution and initial scalar type checking after a successful parse. It reports
duplicate bindings, invalid declaration/control-flow contexts, unknown value/type names and conflicting
declarations. It retains bindings for forward references, generic scopes,
shadowing and overload sets. Scalar checks cover built-in annotations, literal
ranges, variable inference, operators, Boolean conditions, parameter defaults,
explicit returns and assignments, with retained type facts. File-level function
calls check argument types, labels and defaults, select scalar overloads, and
retain the target and parameter mapping. Expression-bodied returns infer scalar
types, including forward calls. `parse` continues to report syntax only.
Numeric conversions validate built-in identity, arity, operand types and widening
ranges. Checked integer conversions return `T?`, with optional type facts retained
through bindings, signatures and calls. Transparent non-generic aliases expand
to built-ins, optional wrappers or nominal declaration identities. Expansion
checks cycles before value typing and supports known direct alias-qualified
type members and numeric conversion targets. Canonical descriptors represent
fully known arrays, pointers, ownership wrappers, function signatures and
non-generic nominal instances. Array literals/indexing, exact composite matches,
primary constructor arguments and typed indirect calls are checked. Direct fields
and methods on known nominal receivers retain declaration/overload identities;
method calls check arguments and infer returns. Static/instance access is checked
through types, aliases, ordinary instances and implicit receivers. Non-generic
hierarchies validate base kinds, finality and cycles. Inherited member/type lookup,
method overrides, supported interface/abstract method requirements and nominal
upcasts retain hierarchy and conformance-witness records. Explicit concrete generic
applications substitute transparent aliases, constructor/function signatures and
direct fields/methods of owners without bases; nominal arguments are invariant.
Supplied arguments infer type parameters for functions, methods and nominal
primary constructors. Concrete interface bounds and `where` clauses require
declared ancestry and compatible supported implementations. Generic bodies use
symbolic parameters and declared bounds once; associated projections and `Self`
substitute for concrete calls and supported witnesses. General inference/bound solving, generic inheritance,
function-value inference, associated defaults, remaining numeric policies,
remaining dependent/property contracts, optional/ownership-wrapper member access and ownership analysis remain
pending, so passing `check` does not yet establish program validity. After typing succeeds, `check`
also reports missing returns, reads of uninitialized locals, assignments to immutable bindings and fields,
`mut fun` calls on immutable receivers,
and checks used `if`/`match`/block results against their expected type. An `if` without `else`
produces `none` where an optional is expected. Imports explicitly report that
package loading is unavailable.

`gate <file.phs>` runs `check`, then reports every construct that stage0's
first native backend cannot compile yet. That backend covers Bool, integer and
String values, plain struct values with field reads and assignment, functions,
locals, loops, calls, integer arithmetic and `print`, and needs a
`fun main()` that returns `Int` or nothing. See
[spec 3.25](docs/spec/03-types-and-semantics.md) for the full subset.
`mir <file.phs>` prints the typed MIR for a file that passes `gate`. The MIR
has basic blocks, typed registers and checked arithmetic, and it lowers code
strictly left to right (spec 3.26).
`asm <file.phs>` prints the x86_64 code that stage0 generates from that MIR as
an assembly listing, with checks that trap on overflow and division by zero.
`build <file.phs> [-o <out>]` links that code with a small stage0 runtime into
a static Linux x86_64 executable that needs no C library. The program's exit
status is the value `main` returns, and an overflow or division by zero
prints a panic message and exits with status 101 (spec 3.26). The prelude
`print` writes a `String`, `Int`, `UInt` or `Bool` and a newline to standard
output (spec 3.27).

```phantasos
fun main() {
    print("Hello, world!")
}
```

## Repository layout

| Path | Purpose |
|---|---|
| `docs/spec/` | Language and ABI specification |
| `docs/adr/` | Architecture decision records |
| `bootstrap/stage0/` | C++23 bootstrap compiler: lexer, parser, structural validation, lexical name resolution and scalar typing; full type checking, native code generation and build driver planned |
| `compiler/` | Self-hosted compiler (a Phantasos workspace), planned |
| `runtime/` | `rt`, the privileged runtime, planned |
| `std/` | Standard library (privileged), planned |
| `libc-compat/` | Optional C standard library for C sources, planned |
| `platform/` | Our own C declarations of OS interfaces |
| `tools/` | Developer tools (`license-check`, ...) |
| `tests/` | Conformance and golden tests |

## Building stage0

Requirements: clang++ 18+, a compatible C++23 standard library, CMake 3.28+,
and Ninja. CMake compiles and links a feature probe for `std::expected`,
`std::print`, `std::format`, `std::ranges::to`, `std::views::join_with`, and
floating-point `std::from_chars`; the Clang version alone is not sufficient.

CI uses Clang 19 with libstdc++ 14 on Linux, and Clang with the MSVC STL on
Windows. [Clang 19 enables libstdc++'s `std::expected`](https://releases.llvm.org/19.1.0/tools/clang/docs/ReleaseNotes.html#c-20-feature-support),
and [libstdc++ 14 adds the required print and range facilities](https://gcc.gnu.org/onlinedocs/libstdc++/manual/status.html#status.iso.2023).
On Ubuntu 24.04 x86_64, install and select the Linux pair explicitly:

```sh
sudo apt-get install clang-19 g++-14 ninja-build
cmake -S bootstrap/stage0 -B build/stage0 -G Ninja -DCMAKE_CXX_COMPILER=clang++-19 -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS=--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/14
```

For Windows, or an already configured compatible toolchain:

```sh
cmake -S bootstrap/stage0 -B build/stage0 -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug
cmake --build build/stage0
ctest --test-dir build/stage0 --output-on-failure                    # focused C++ library tests
python tests/run_golden.py --stage0 build/stage0/phantasos-stage0      # golden tests
python tests/test_frontend.py --stage0 build/stage0/phantasos-stage0   # input limits, nesting and parser state
python tests/fuzz_frontend.py --stage0 build/stage0/phantasos-stage0   # crash/hang fuzzing
python tests/fuzz_frontend.py --stage0 build/stage0/phantasos-stage0 --command check # semantic-path fuzzing
python tests/fuzz_frontend.py --stage0 build/stage0/phantasos-stage0 --command gate  # code-generation-gate fuzzing
python tests/fuzz_frontend.py --stage0 build/stage0/phantasos-stage0 --command mir   # MIR lowering fuzzing
python tests/fuzz_frontend.py --stage0 build/stage0/phantasos-stage0 --command asm   # x86_64 code generation fuzzing
python tests/fuzz_frontend.py --stage0 build/stage0/phantasos-stage0 --command build # executable linking fuzzing
python tools/license-check/license_check.py                            # license headers
```

The build separates the `phantasos-stage0` CLI from the `phantasos_frontend`
static library (CMake alias `Phantasos::Frontend`). Link the library to reuse
source management, diagnostics, lexing, parsing, AST dumps, structural
validation, name resolution and scalar typing. Its target
propagates the required C++23 and no-exception/no-RTTI settings. Focused C++
tests are enabled by default; use `-DBUILD_TESTING=OFF` to omit them, following
[CMake's CTest convention](https://cmake.org/cmake/help/v3.28/module/CTest.html).

`FrontendSession` in `frontend/session.hpp` owns source files, keyword-seeded
interning, diagnostics, AST storage and cached name-resolution/type results.
Add or load a file through `session.sources()`, then call `session.parse(id)`
or `session.check(id)`. Query `session.diagnostics().has_errors(id)` for that
file's result. Errors in other files do not block checking, and repeated calls
do not duplicate diagnostics. Returned AST and resolution pointers remain
valid until the session is destroyed. `CheckResult::scalar_types` is non-null
when scalar checking ran; query expression and binding facts through it.
Its `signature(BindingId)` and `call(CallExpr*)` queries expose scalar parameter
and return types, selected function IDs, and argument-to-parameter mappings.
Selected calls retain explicit/inferred type arguments and substituted parameter
types; member records retain their concrete receiver type. Declaration signatures
remain shared and are never overwritten by an application's types.
Generic signatures retain symbolic type parameters/projections. `body(BindingId)`
reports the supported schema check as resolved, deferred or erroneous. Associated
type/property witnesses retain their substituted type as well as the implementation.
`constraints()` retains resolved, deferred or erroneous application contracts,
including concrete type applications outside calls. Failed constraints report E0471.
Function value bindings remain deferred; their return types live in signatures.
`ScalarType` stores `kind`, `optional_depth` and a result-local descriptor ID.
Use `descriptor(type)` for structural components and
`type_name(type, resolution, names)` for an iterative complete rendering;
`scalar_type_name` renders built-ins and fallback kind labels. `conversion(CallExpr*)` records
conversion mode and source/destination types. Intrinsic conversion call targets
refer to built-in type bindings, while ordinary call targets refer to functions.
`alias(BindingId)` exposes an alias's expansion state, canonical target ID,
type fact and optional depth. Nominal identity includes the canonical declaration
and concrete generic arguments; dependent inheritance/conformance remains pending. Alias definition errors
retain these records and skip value checking for that file.
`ScalarType::Deferred` means the type needs a later pass, while `Error` marks a
diagnosed type failure. Numeric defaults are `Int` and `Float64`; a scalar
annotation or a typed operator operand can constrain literals. Stage0 currently
uses 64-bit `Int`/`UInt` for its initial x86_64 targets. Files still resolve
independently; this does not implement package or module loading.

Source files are limited to **64 MiB of raw input** by default, before BOM and
line-ending normalization. Oversized files are rejected before reading their
contents; the CLI returns exit code 2 for source-loading failures. Library
callers can configure `SourceLimits` (byte and file-count limits), and handle
`SourceErrorKind` through the `std::expected` returned by both `load` and `add`.
The hard byte ceiling is `UINT32_MAX - 1`, leaving room for EOF and 1-based
locations. These are input limits, not a total compilation-memory budget.
Source references and text views remain valid when more files are added.

Stage0 limits recursive grammar nesting to 128 active guarded entries, shared
across expressions, types, patterns, blocks, and declarations. Exceeding this
implementation limit reports `E0112` and stops parsing that file. Flat operator
and member chains are parsed iteratively; AST dumps also use explicit work
stacks, including inline type and pattern rendering.

Try it:

```sh
build/stage0/phantasos-stage0 parse tests/stage0/parser/showcase.phs   # print the syntax tree
build/stage0/phantasos-stage0 lex   tests/stage0/lexer/literals.phs    # print the token stream
build/stage0/phantasos-stage0 gate  tests/stage0/codegen/gate_fib.phs  # check a program for code generation
build/stage0/phantasos-stage0 mir   tests/stage0/mir/fib.phs          # print its MIR
build/stage0/phantasos-stage0 asm   tests/stage0/asm/fib.phs          # print its x86_64 code
build/stage0/phantasos-stage0 build tests/stage0/run/fib.phs -o fib   # build a Linux executable
./fib; echo $?                                                         # prints 42
build/stage0/phantasos-stage0 build tests/stage0/run/hello.phs -o hello
./hello                                                                # prints Hello, world!
```

## License

Apache License 2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE). Every source
file must start with the project license header, and CI enforces this with
`tools/license-check`.
