# 3. Types and semantics

## 3.1 Built-in types [Core]
| Type | Description |
|---|---|
| `Int8 Int16 Int32 Int64`, `UInt8 ... UInt64` | Fixed-width integers. Arithmetic **traps on overflow**; the `&+ &- &*` forms wrap. |
| `Int`, `UInt` | Pointer-sized integers (64-bit on x86_64, 32-bit on i386) |
| `Float32`, `Float64` | IEEE-754 floating point |
| `Bool` | `true` / `false` |
| `Char` | One Unicode scalar value (32-bit) |
| `String` | Owned, immutable UTF-8 text |
| `[T]` | Owned, growable array (`std.Array<T>`). Indexing is bounds-checked. |
| `T?` | Optional value: either `none` or a `T` |
| `()` | `Unit` |
| `Never` | The type of expressions that never return (`panic`, `throw`, infinite `loop`) |

There are **no implicit numeric conversions**. Widening is written
`Int64(x)`, and narrowing uses `Int8.checked(x)` or `Int8.truncating(x)`.

## 3.2 Type kinds
- **`struct`**: a value type with no identity. Fields are immutable unless
  declared `var`.
  - Copying is implicit when every field is `Copy` (automatically derived).
    Otherwise the struct is move-only.
  - `struct S: ~Copy` opts out of copying explicitly.
  - Plain parameters in a struct's primary constructor become `let` fields.
- **`class`**: a reference type with identity, heap-allocated, with dynamic
  dispatch through a vtable.
  - **Final by default.** `open class` allows subclassing, and `open fun`
    allows overriding. An override must say `override`.
  - `abstract class` cannot be instantiated and may declare methods without
    a body.
  - `sealed class` and `sealed interface` can only be subclassed or
    implemented within the same package, so `match` over them can be
    checked for exhaustiveness.
  - Only primary-constructor parameters marked `let` or `var` become fields.
- **`enum`**: a sum type. Each case can carry a payload, e.g.
  `BadDigit(at: Int)`.
- **`interface`**: a set of requirements. It can declare default method
  bodies and associated types (`type Item`). A class or struct can implement
  any number of interfaces.
- **`actor`**: a reference type whose mutable state is isolated. See ch. 5.
  Primary-constructor parameters marked `let` or `var` become fields, as in
  classes; plain parameters do not.

## 3.3 Mutability [Core]
- `let` creates an immutable binding and `var` a mutable one. Fields follow
  the same rule.
- A method can only mutate `self` if it is declared `mut fun`. Which
  receivers may call it depends on the kind of type:
  - **Value types** (`struct`, `enum`): the receiver must be a mutable place,
    i.e. a `var` binding, a `var` field of a mutable place, an `inout`
    parameter, or `self` inside another `mut fun`. A `let`-bound value can
    never be mutated.
  - **Class instances**: the binding's `let`/`var` only controls whether the
    binding can be reassigned. A `mut` method may be called through any
    reference that is not read-only: an owning binding (`let` or `var`), a
    `shared` handle, an `inout` or `own` parameter, or `self` inside another
    `mut fun`. It may **not** be called through a `borrow` parameter
    (`borrow` is the default when a parameter has no convention) or through
    `self` inside a non-`mut` method, because a borrow grants read-only
    access (3.4).
- Immutability is **deep for value types**. For class references it is
  shallow: a `let` binding holding a class reference cannot be reassigned,
  but the object can still be mutated through its `mut` methods (by the rule
  above) if its fields are `var`.
- A type is **deeply immutable** when no state reachable from it can ever
  change. The definition is recursive:
  - primitive types, `String`, `()`, and `Never`;
  - `T?` and `[T]` when `T` is deeply immutable;
  - value types whose fields all have deeply immutable types;
  - `shared T` when `T` is deeply immutable;
  - classes that are **not `open`**, whose stored fields, including
    inherited ones, are all `let` and all have deeply immutable types.

  `var` fields, `weak T`, raw pointers, and `open` classes are never deeply
  immutable; an `open` class is excluded because a subclass could add mutable
  state. Recursive types are checked coinductively, so a class may refer to
  itself through a `let` field, for example `let next: shared Node?`.
  Deeply immutable types are automatically `Sendable` (ch. 5).

## 3.4 Ownership, borrowing, and destruction
- **Ownership.**
  - Every value has exactly one owner.
  - Assigning a non-`Copy` value, or passing it to an `own` parameter,
    **moves** it. Using a moved-from binding is a compile-time error.
  - Class instances created with `ClassName(...)` are uniquely owned.
- **Borrowing.**
  - Parameters are `borrow` by default: read-only access for the duration of
    the call. `inout` gives exclusive read/write access for the duration of
    the call.
  - Borrows are **second-class**. They cannot be stored in fields, returned,
    or captured by escaping closures, so no lifetime annotations are needed.
  - While an `inout` borrow is active, its source cannot be accessed any
    other way (the law of exclusivity).
- **Sharing.**
  - `shared T` is a reference-counted handle; `shared expr` converts an owned
    value into a shared one.
  - `weak T` is a non-owning reference that can become `none`.
  - The compiler uses non-atomic reference counting unless a value is used
    across tasks or actors (see `Sendable`).
- **Destruction.**
  - A value is destroyed when its owner goes out of scope without having been
    moved, or when the last `shared` handle is released.
  - Destruction runs the type's `deinit`, then destroys the fields in
    reverse declaration order, then runs the superclass destructor chain.
  - A `deinit` cannot throw and cannot let `self` escape.

## 3.5 Construction (two-phase initialization)
1. **Phase 1.** The initializer assigns every stored field declared by its
   own class. Reading `self`, calling methods, or letting `self` escape is a
   compile-time error during this phase.
2. **Delegation.** The initializer calls `super(...)`, either implicitly
   through the superclass entry in the type head or explicitly. That
   initializes the superclass fields.
3. **Phase 2.** `self` is fully formed, and methods, including virtual ones,
   may be called.

Definite-initialization analysis enforces this. A virtual call can never
reach an object whose subclass fields are uninitialized.

A failing initializer is declared `init(...) throws(E)`. If it throws, only
the fields that were already initialized are destroyed.

## 3.6 Optionals and absence of null [Core]
- Phantasos has no null. `T?` holds either `none` or a `T`.
- The ways to use an optional are `if let x = opt`, `opt?.member`,
  `opt ?? fallback`, `match`, and `opt!`. `opt!` panics with a source
  location if the value is `none`.
- **Smart casts.** After `if x is Circle`, the name `x` has type `Circle`
  inside the branch, as long as `x` is an immutable binding.

## 3.7 Errors [Core]
- `fun f(): T throws(E)` means `f` either returns a `T` or throws an `E`. A
  bare `throws` means `throws(any Error)`. A function with no `throws` clause
  cannot throw (it is `throws(Never)`).
- Every call to a throwing function must be marked with `try`, which
  propagates the error, or `try?`, which turns it into an optional. The
  compiler checks that each propagated error type converts to the caller's
  error type.
- Handling an error at the call site (`expr catch e { ... }`) is part of the
  full language; its grammar is still to be specified.
- Lowering: errors are returned as a tagged result. There is **no
  unwinding** and no exception tables.
- **Panics** (failed bounds checks, overflow, `!` on `none`, explicit
  `panic()`) are not catchable. They print a message and a backtrace, then
  abort the process.

## 3.8 Visibility [Core]
| Modifier | Visible to |
|---|---|
| `private` | the enclosing type (and its extensions in the same file) |
| `file` | the same source file |
| `internal` *(default)* | the same package |
| `public` | importers of the package |

A declaration is never more visible than its enclosing type. A public
signature cannot mention less-visible types.

## 3.9 Privileged internals
- The module `phantasos.internal` provides compiler intrinsics, raw
  syscalls, runtime hooks, and unchecked operations.
- Importing it is a **hard error** unless the importing package is
  privileged. The privileged packages are `std`, `rt`, and `libc-compat`:
  they ship in the toolchain directory, and their content hash must match
  the toolchain manifest.
- Internal items are mangled with the reserved `_P$` prefix and emitted with
  hidden visibility.
- Re-exporting internal items, or exposing them through public generic
  signatures, is rejected.

## 3.10 Unsafe code
- Raw pointers (`*T`, `*mut T`), `extern "C"` calls, and unchecked
  intrinsics are only allowed inside `unsafe { }` or in an `unsafe fun`.
  Calling an `unsafe fun` requires an `unsafe` block.
- Outside `unsafe`, a program never has undefined behavior.

## 3.11 Generics
- Type parameters are bounded by interfaces: `<T: Comparable & Hashable>`,
  or with a `where` clause.
- Generic code is checked once, against its bounds (not after
  instantiation), and then **monomorphized**.
- The full language may share generic code between class-typed
  instantiations.

## 3.12 Binding and control-flow constraints [Core]

- Parameter prefixes `let` and `var` are allowed only in primary
  constructors. Ordinary functions, methods, extern functions and secondary
  `init(...)` declarations cannot use them; stage0 reports `E0207`.
- Names must be unique within each parameter list, generic parameter list,
  or block's local bindings. A nested scope may shadow an outer binding;
  the function body is a nested scope of its parameters. Enum payload
  labels must be unique within a case. Named non-function value declarations
  cannot repeat in one declaration list, nor can type declarations or aliases.
  These checks do not determine which function overloads are valid.
  Promoted primary-constructor fields share the member value namespace:
  `let`/`var` parameters in classes and actors, and all parameters in structs,
  must not collide with explicit member value declarations.
- A name may be bound only once within a pattern. All alternatives of an
  alternative pattern must bind the same set of names, so the arm's guard
  and body can refer to those names regardless of the chosen alternative.
  For example, `.A(x) | .B(x)` is valid, while `.A(x) | .B(y)` is not.
- `init` and `deinit` must be type members. A type has at most one primary
  `init` body (the form without a parameter list) and one `deinit` body.
  Secondary constructor overloads are checked separately.
- `return` requires an enclosing function, initializer or destructor body.
  `break` and `continue` require an enclosing loop **body** in that callable;
  a loop's own condition or iterable expression is outside its body.
- An explicit `throw` must be handled by an enclosing `try?` expression or
  propagated from a function/initializer with a `throws` clause. A `try?`
  expression handles errors only from its operand, not later statements.
  Error-type compatibility and throwing-call checks still follow section 3.7.
- `await` requires an `async` function body. Defaults, attributes, field
  initializers and superclass arguments do not inherit the function body's
  async or throwing context. See chapter 5 for further concurrency rules.
- `defer` schedules synchronous cleanup on leaving the enclosing scope, in
  reverse registration order. Its body cannot `return`, jump to an enclosing
  loop outside the cleanup, propagate an error, or suspend with `await`.
  Loops nested inside a `defer` may use `break`/`continue`, and `try?` inside
  it may handle local errors. Each nested `defer` starts a fresh cleanup
  boundary. Throwing-call validation remains part of later effect analysis.

Stage0 validates these structural constraints and the type-alias forms in
chapter 2 before running the name resolver below. Initial typing and hierarchy
checks follow in 3.14–3.20; remaining typing, visibility, all-path return,
ownership and effect analysis remain on the [roadmap](../roadmap.md).

## 3.13 Lexical name resolution [Core]

- Types and values have separate namespaces. Type declarations, aliases and
  generic parameters bind type names; functions, variables, fields, enum
  cases, parameters and patterns bind value names. In an expression, lookup
  checks the value namespace first, then the type namespace at each scope
  (for constructor calls and type-qualified members), before visiting the
  parent scope. Type expressions only search the type namespace.
- File and type-member declarations are visible throughout their declaration
  list, including before their definition. `extern` blocks add functions to
  the surrounding scope. Functions sharing a name form an overload set;
  mixing functions with other values in the same scope is an error. Type
  and value declarations may share a spelling. Choosing an overload and
  validating its signature belong to type checking.
- All generic parameters are visible in their bounds, `where` clauses,
  signatures and bodies. A nested declaration may shadow outer generic
  parameters. Type aliases have their own generic scope; associated types
  are visible to other members of their interface.
- Parameters are visible in the body. A default expression sees earlier
  parameters, plus the enclosing declaration scope, but not its own or later
  parameters. Primary-constructor defaults see type generics and the outer
  scope, not member fields. Primary-constructor parameters are visible to
  superclass arguments, instance field initializers and the primary `init`
  body; only promoted fields are visible to other members. Secondary `init`
  bodies have their own parameter scope.
- A local binding becomes visible after its initializer. Thus `let x = x`
  uses an outer `x`, or reports an unknown name if none exists. A block adds
  a nested scope, so locals can shadow parameters and outer locals. A
  `defer` body resolves names where it is declared; later locals are not
  visible inside it.
- `if let` and `while let` bindings are visible only in their body, not in
  the condition or the `else` branch. A `for` binding is visible in its body,
  not its iterable. Each match arm has its own scope: its pattern bindings
  are visible in the guard and body. Matching alternatives share bindings;
  case names and pattern types are looked up outside the arm's bindings.
- `Self` denotes the enclosing type. `self` requires an instance function,
  initializer or destructor body; `super` additionally requires a declared
  base type. Static bodies and parameter defaults do not have a receiver.
  Static/instance access and initial inheritance checks run in the type pass
  (3.19–3.20). Initialization safety remains later work.

Stage0 retains immutable scope, declaration, overload-set and reference
records separately from the AST. References are explicitly resolved,
unresolved (with a diagnostic), or deferred. It resolves directly declared
nested types and type-qualified values. Receiver-dependent members, implicit
`.Case` names, alias-qualified names and dependent generic members such as
`T.Element` are deferred until type information is available. Inherited
lookup is also deferred, including an unqualified name that a base could
provide before an outer scope is searched. A type's own generic parameters
take precedence over that inherited lookup. Lexical lookup alone cannot
select inherited members or check visibility. Non-generic alias expansion and
cycles are checked by the type pass (3.17); initialization cycles remain pending.

The bootstrap prelude supplies type names `Int8/16/32/64`, `UInt8/16/32/64`,
`Int`, `UInt`, `Float32/64`, `Bool`, `Char`, `String`, `Unit`, `Never`, `Error`,
`Copy` and `Sendable`, plus the value name `panic`. These are name bindings,
not implemented runtime definitions or checked signatures. User declarations
may shadow them. Other library names must be declared explicitly.

The current API resolves one file. Package discovery, shared module scopes
and imported declarations belong to P2. `check` reports `E0304` for imports;
`parse` still accepts their syntax. Attribute names, argument keys and call
argument labels are metadata, while attribute argument values undergo the
same expression lookup as other expressions.

## 3.14 Scalar typing and bootstrap coverage [Core]

Integer literals default to `Int`, and floating-point literals to `Float64`.
An explicit scalar annotation or a concrete operator operand constrains numeric
literals, including parenthesized expressions and trees of numeric operators.
An integer literal takes an integer type; a floating-point literal takes a
floating-point type. A literal's magnitude must fit its chosen type; a directly
negated integer literal (possibly parenthesized) may use the signed minimum's
magnitude. `Float32` literals must fit its finite range. Stage0 checks range,
not exact floating-point representability. Arithmetic overflow remains a
runtime trap; this pass does not fold constant expressions.

Unannotated variables infer their initializer's type, then default any remaining
numeric literals. That type is fixed before uses in other expressions: a binding
inferred as `Int` cannot be assigned to `Int64` implicitly, even on x86_64.
`Int`/`UInt` remain distinct from `Int64`/`UInt64`; stage0's initial Windows x64
and Linux x86_64 targets use 64-bit ranges for them. User types that shadow
built-in names retain their own identity.

Discarded numeric expressions also default and undergo range checking. These
include expression statements before a block's final statement, and all
expression statements in ordinary function, initializer, destructor, loop and
cleanup bodies. If a block's result is used, its final expression retains its
deferred contextual type. Discarding a call does not default its arguments.

Scalar arithmetic (`+ - * /`) requires operands of the same numeric type.
Remainder, wrapping arithmetic, shifts and bitwise operators require the same
integer type. Unary `-` requires signed numeric operands, and `~` requires an
integer. `!`, `&&`, `||`, ordinary `if`/`while` conditions and match guards
require `Bool`. Scalar equality accepts matching built-in types; ordering
accepts matching numeric types or `Char`. There are no implicit numeric
conversions between typed operands. Compound assignment follows the same
numeric/integer domains and requires the target's type.
Boolean, numeric, integer and ordering requirements are checked independently
for each available operand, even when the other operand's type is deferred.
Compound assignment checks both the target and source domains before type
compatibility. Matching operand types and inferring results can still remain
deferred.

Scalar variable annotations, parameter defaults, assignments and explicit return
values must match their expected type. A promoted primary-constructor field
retains its parameter's declared type in a separate field binding.
A block-bodied function without a return annotation returns `Unit`;
an expression-bodied function infers its result from the body, defaulting
numeric literals at the function boundary.
An explicit bare `return` produces `Unit`, with the same optional lifting as
`return ()`. Initializers and destructors also
return `Unit`. This does not establish that every path returns a value.

After successful validation and name resolution, stage0 checks these scalar
rules and retains expression/binding types. `E0400` reports mismatches,
`E0401` invalid operator domains, and `E0402` out-of-range literals. A type error
does not alter the syntax tree, and dependent expressions avoid cascading
diagnostics. Parsing alone performs none of these checks.

The type API explicitly returns `Deferred` for unsupported calls, named-function values,
generic types and aliases, inherited/dependent members (3.19), optional
bindings, casts, effect operators and block/branch results. It traverses their
children to check supported scalar subexpressions. Forward variable references
can infer scalar types. Numeric literals in deferred contextual positions keep
deferred types instead of defaulting prematurely (for example, an unchecked
call argument could require `UInt64` rather than `Int`). Cyclic inference
without an established type remains
deferred. Generic calls, function-value inference, remaining numeric policies, initialization cycles,
all-path returns, mutability, ownership and effects still need later passes.
Successful `check` therefore continues to mean only that implemented checks
found no errors, not that the program is valid or executable.

## 3.15 Direct scalar function calls [Core]

Stage0 retains scalar parameter and return types for function declarations,
including extern functions. Function values themselves are not scalar: their
value bindings remain deferred, and return types are queried through signatures.
An omitted return annotation means `Unit` for a block body or declaration
without a body. Expression bodies infer their scalar result, including forward
calls and references. Recursive inference cycles without an established type
remain deferred; an explicit return annotation can break such a cycle.
Calls select targets from argument facts before adding return dependencies.
Only the selected target's return type participates, so eliminated overloads
cannot create artificial inference cycles.

Direct calls to file-level functions check supplied arguments against the
declared parameters. Positional arguments form a prefix in declaration order;
named arguments follow them and may be reordered. Labels name parameters.
A parameter may be supplied only once. Every omitted parameter must have a
default; defaults are checked once in their declaration's scope. A selected
call retains a function binding ID and the parameter index of each supplied
argument. Default insertion/evaluation is left to lowering.

Overload selection considers argument shape and types, without using the
expected result type. Typed scalar arguments require exact types, with no
implicit numeric conversions; `Never` can stand in for an expected type.
Numeric literal trees take a matching integer/float parameter type when their
literals fit its range. Among fully known candidates, prefer the fewest literal
types differing from the defaults `Int` and `Float64`. Remaining ties are
ambiguous; declaration order and return types never break a tie. Speculative
probes do not emit diagnostics or change literal types. Only the selected
candidate contextualizes its arguments. If a unique otherwise matching
signature has a range failure, report the literal's precise range/operator
diagnostic.

A potentially matching generic/constrained signature, unsupported parameter type or pending
argument type keeps selection deferred, even if another scalar candidate fits.
Independently provable shape/type mismatches still eliminate candidates. Calls
through generic instantiations remain deferred. Direct nominal methods and
type-qualified calls follow 3.19.
Known function-typed calls and primary constructors follow 3.18. Built-in numeric conversions follow section 3.16. The pass checks
their supported scalar children. Constraints, effects, conventions, visibility, duplicate
signature validity and all-path returns require later checks.

`E0410` reports calling a known scalar value, `E0411` no matching signature,
and `E0412` an ambiguous call. Candidate notes point to declarations. Return
types, call states and selected targets are cached with other scalar facts;
failed calls suppress dependent type diagnostics. These checks preserve the AST
and use iterative dependency traversal, including long forward call chains.

## 3.16 Explicit numeric conversions and optional facts [Core]

The numeric conversion APIs require exactly one positional argument. The callee
must resolve to a built-in numeric type; declarations that shadow a built-in
name keep their own meaning. Aliases expanded to plain built-in numeric types
can name the same intrinsics. Conversion type checking does not execute conversions or fold their
values; execution and lowering remain future compiler work.

- `T(value)` is an identity or lossless widening conversion. Integers of the
  same signedness can widen to equal or greater width; unsigned integers can
  widen to signed integers only at strictly greater width. Signed integers
  cannot widen to unsigned integers. `Int`/`UInt` use the initial x86_64 ranges
  but retain distinct identities from `Int64`/`UInt64`.
- Floating identity and `Float32` to `Float64` widening are supported.
- A numeric literal tree in a widening constructor takes the destination's
  matching integer/float type and must fit that range. This is literal typing,
  not narrowing an already typed value.
- `T.checked(value)` for integers returns `T?`: the converted value on success,
  or `none` when the source value is outside `T`'s range.
- `T.truncating(value)` for integers returns `T`: reduce the value modulo
  `2^N`, then interpret the bits using the destination's signedness, where `N`
  is its width. Checked and truncating operations accept any integer source
  width/signedness. Their numeric literal inputs default to `Int`; an explicit
  source constructor such as `UInt64(...)` can supply a wider literal value.

Optional wrappers on built-in types retain their full identity in type facts,
including signatures and inferred checked results. A value of `T` can be lifted
to `T?` (and deeper wrappers) in an expected optional context; this does not
permit numeric conversions between different underlying built-ins. Calls prefer
fewer optional lifts after numeric literal preferences. Checked results cannot
be used as ordinary numeric values: unwrap a known optional with `!` before
numeric use. Unwrapping retains its element type; runtime failure handling is
defined in section 3.6. None inference, conditional bindings, optional equality,
coalescing and other optional operations remain later checks.

Integer/floating interchange and floating narrowing remain explicitly deferred
until their rounding and failure policies are specified. Pending source types
also keep conversion results deferred, while independently invalid shape or
known non-numeric/optional operands are diagnosed. `E0420` reports conversion
argument shape, `E0421` invalid operands, and `E0422` a typed narrowing request in
a widening constructor. Existing literal range/operator diagnostics still apply.

Conversion records retain the mode, source/destination types and checked state.
Their call targets identify built-in types rather than functions; they introduce
no function-return dependency. All facts remain cached and the AST is unchanged.

## 3.17 Transparent alias expansion [Core]

A non-generic `type Name = Target` is transparent: it does not create a new
type identity. Stage0 expands chains in their declaration scopes using retained
lexical bindings. Forward declarations and aliases nested in nominal types are
supported. A use-site binding with the same spelling cannot capture names in
an alias's definition.

Expansion retains the terminal built-in or non-generic nominal declaration ID.
Built-in facts and optional depths pass through aliases unchanged; aliases to
the same type are identical for scalar annotations, calls and overloads.
Nominal targets also retain optional wrappers. Their canonical declaration IDs
are used for initial nominal checking (3.18). Aliases naming plain built-in numeric types support the
conversion APIs in 3.16 and use the canonical built-in as the intrinsic target.
Value bindings/functions still take precedence over type names at value uses.

Known direct type members can be looked up through a nominal alias, including
another alias. This lookup respects separate type/value namespaces and does not
rewrite the lexical resolver's deferred references. Missing direct members of
a non-inheriting nominal owner report `E0301`; known inherited paths are checked
in 3.20, while dependent lookups remain deferred. Annotation diagnostics are cached by syntax node, so generated
parameter/field pairs do not duplicate them.

Transparent aliases must be acyclic. `E0430` reports an expansion cycle with
notes on its path, including cycles through optional, array, pointer, function
and generic-argument syntax, or through a known alias-qualified member. Only
non-generic transparent definitions participate in this initial cycle pass.
Alias-qualified dependency discovery examines nested paths in all of these
forms, even when the composite expansion result remains deferred.
Dependent aliases retain an error fact without additional cycle diagnostics.
Alias definition errors gate value checking for that file while retaining alias
records; `parse` remains syntax-only. Traversal uses explicit work stacks.

Generic aliases/instantiation, captured generic types and associated requirements
and their overrideable defaults remain deferred. Fully known composite targets
normalize into canonical type descriptors (3.18).
These forms are not unconditional transparent expansion edges in this pass.
Unsupported forms still retain their declarations for later type checking.

## 3.18 Initial composite and nominal type checking [Core]

Type facts use a result-local canonical descriptor table for arrays, pointers,
`shared`/`weak` wrappers, function signatures and non-generic nominal instances.
Descriptors refer to other type facts by ID, not recursive owning objects.
Aliases reuse the same identities. Nominal identity is the canonical declaration
ID, including its enclosing scope; equal spellings in different scopes do not
create equal types. Optional wrappers preserve descriptor identity. Generic
instances and types capturing generic parameters remain deferred.

Fully known composite annotations, parameters and return types participate in
exact matching and optional lifting. Arrays are invariant; pointer mutability
is part of identity. Function descriptors include parameter/result/error types
and the throws flag. Nominal inheritance/conformance follows 3.20. Function
variance/effect coercions and ownership-wrapper conversions remain deferred;
they are not assumed to succeed or treated as proven mismatches. Unrelated
concrete nominal types and scalar-to-concrete-nominal mismatches are rejected.
Function compatibility compares parameters contravariantly and results
covariantly before ruling out a possible conversion.

Array literals take an expected `[T]` context when available, including nested
and parenthesized literals. Otherwise a concrete element can anchor inference.
Concrete anchors propagate through nested literals and take priority over
numeric defaults from siblings. Numeric literals default to `Int`/`Float64`.
Element types must match without implicit numeric conversion. `Never` elements
do not anchor inference. Empty and `Never`-only nested literals can take context
from a sibling; they do not
suppress errors in that sibling. Without an anchor or expected type, the array
remains deferred. Unresolved concrete element types also remain deferred.
Overload probes inspect elements without changing them,
and only the selected signature contextualizes its literal arguments.
Array equality remains deferred, but independently inferred literal operands
still undergo element validation.

Indexing a known non-optional array requires an integer index (or `Never`) and
produces its element type. Invalid array/index combinations report `E0440`.
This does not perform constant bounds evaluation; runtime bounds checks and
lowering are pending. String, pointer and nominal indexing remain later work.

Non-generic classes, structs and actors check primary constructor arguments,
labels and defaults using the same type rules as function calls. The result is
the nominal instance type. Abstract classes and interfaces cannot be constructed
directly (`E0441`). Secondary constructor overloads, enum cases, constraints,
initialization ordering, ownership and construction effects need later checks.
Independently provable constructor shape/type/range errors are still diagnosed.

Known non-optional function-typed values check positional argument count and
types and retain the declared result type; such signatures have no argument
labels or defaults. Named-function value inference and contextual overload
selection for function values remain deferred. Direct named calls still select
from argument facts before traversing only the chosen return dependency.

Call records distinguish direct functions, numeric intrinsics, nominal primary
constructors and typed indirect functions. The latter two finish without adding
function-return dependencies. Indirect calls may identify a parameter/variable
binding, or have no static binding ID for a computed callee. Type normalization,
formatting, literal materialization and matching use explicit work stacks,
including across long alias chains. Matching visits each directed descriptor
pair once per comparison. Completed type/descriptor facts are cached.
Type-name formatting also bounds expansion work and output: public names use
a default 32 KiB limit, and diagnostic names use 1 KiB per type. Incomplete
names end in `...`; smaller explicit API limits shorten that marker as needed.
This prevents shared descriptor graphs from producing exponential diagnostics.
Descriptor pointers are stable after checking, but not across mutable interning.

Receiver-dependent field/method lookup follows 3.19–3.20. Generic conformance,
smart casts, effects, unsafe checking and remaining optional operations are pending.
Passing `check` continues to establish only the implemented checks.

## 3.19 Initial receiver-dependent members [Core]

Known non-optional, non-generic nominal receivers support directly declared
fields and methods. Promoted primary-constructor fields keep their separate
binding identities and declared types. Explicit field initializers can infer
their types. Field reads, chains and assignment type requirements use those
facts; initialization, mutable-place legality and ownership remain later checks.

Instance members require an instance receiver. Static fields/methods and nested
types require a type receiver, including a transparent alias or `Self`.
Unqualified instance fields/methods use the enclosing implicit receiver when
available; static bodies and parameter defaults have no implicit instance.
Instance field initializers can refer to fields without making explicit `self`
available. Unqualified static members are available in either kind of body.
Values take precedence over nested types with the same spelling.

Method calls use the direct-call argument, label, default, overload and literal
range rules. Only the selected method adds a return-inference dependency;
rejected overloads cannot create inference cycles. A field of function type
uses the positional indirect-call rules. Bound method values and explicit
generic method instantiation remain deferred.

`E0450` reports a missing receiver member when lookup is complete. `E0451`
reports an incompatible static/instance receiver. Lexical type-qualified name
errors retain the resolver's diagnostics. Known inherited lookup and `super`
follow 3.20. Generic owners/captured generic types, optional chaining, ownership-wrapper
and built-in members, visibility, effects and smart casts still require later
checking. Direct field facts of a derived owner can still be used.

`ScalarTypes::member` retains the owner, receiver kind and field/method/nested
type candidates for ordinary member expressions and implicit member names.
Lexical reference states and the AST remain unchanged. `CallKind::Method`
records retain selected targets and explicit instance receiver expressions;
implicit/static receivers have no explicit expression. Lookups, entity caching
and selected dependencies use explicit stacks, including long member/call chains.

## 3.20 Initial inheritance and conformance checking [Core]

Stage0 builds a canonical, file-local graph for non-generic nominal declarations.
Classes may have one class superclass and any number of interfaces; interfaces
extend interfaces, and structs, enums and actors may declare interface bases.
A superclass must be `open`, `abstract` or `sealed`, and cannot be `final`.
Sealed bases in this file are in the same package. Base arguments belong only
to a superclass. Optional/scalar bases, repeated canonical bases, invalid type
kinds and multiple class superclasses report `E0460`. Cycles report `E0461`
with declaration notes. Invalid/incomplete bases propagate to descendants.

Known inherited fields, nested types, aliases and method overloads participate
in lookup. More-derived declarations hide matching inherited declarations;
method input profiles include labels, parameter types and static/instance kind.
Different profiles remain overloads. Diamonds share declaration identities.
Class implementations take precedence over interface defaults, and a more
specific interface declaration hides all of its ancestor declarations. Unrelated
defaults remain ambiguous. Ambiguous non-method members report `E0452`.
Specificity is applied before default-body preference, including through cached
intermediate bases: a bodyless redeclaration still suppresses its ancestor's
default when an unrelated interface supplies the selected body. Base-list order
does not change this suppression.

The type pass retries lexical references deferred at an inheritance barrier.
It searches outer scopes only after complete inherited lookup finds nothing;
thus inherited names still shadow outer names. Type and value namespaces remain
separate. Inherited aliases and newly available base types normalize before
callable signatures; newly exposed alias cycles still report `E0430`. These
operations leave the AST and lexical reference states unchanged.
Qualified type paths cannot traverse an optional alias owner; such paths remain
deferred, including when inherited lookup would otherwise expose a nested type.

Known ancestry permits nominal superclass/interface upcasts and optional
lifting, while downcasts and unrelated known nominal assignments are rejected.
Arrays remain invariant and numeric conversions remain explicit. Overloads
prefer exact argument types; among equal literal/optional ranks, a signature
whose supplied parameter types are uniformly more specific wins. Incomparable
signatures remain ambiguous.
Structurally complete method lookup retains dependent overload candidates.
Calls discard candidates with incompatible argument counts/labels before
deferring selection for applicable generic or otherwise dependent signatures;
an inapplicable generic overload does not suppress checks on a known call.

A class override of an inherited input profile must say `override`, and its
base method must be open/abstract (interface methods are implicitly virtual).
Final methods cannot be overridden. Interface requirement implementations do
not require `override`, but an explicit modifier is checked. New overloads are
not overrides. Known result types may be covariant, throwing implementations
cannot replace non-throwing requirements, and implementations cannot strengthen
a read-only method into a `mut` method. Invalid overrides report `E0462`.

Concrete types must supply a compatible body for each supported interface or
abstract method requirement. A unique inherited default body can satisfy it;
missing, incompatible or ambiguous implementations report `E0463`. Abstract
types can retain unimplemented obligations. Calls still depend on only their
selected method return, including inherited methods and defaults.

`super.member` uses the immediate class superclass. Explicit superclass-head
arguments check primary constructor labels, arity, types and literal ranges.
Secondary constructor selection, implicit delegation, explicit `super(...)`
delegation calls and initialization ordering remain deferred; those calls retain
a known constructor target when available without claiming completed checking.

`ScalarTypes::hierarchy` retains direct bases, the superclass, structural graph
state, conformance state and requirement-to-implementation witnesses. A witness
has an implementation ID only when resolved. Deferred state is not successful
conformance. Generic/captured owners, `where` constraints, associated/property
requirements, interface `Self` substitution, non-default receiver/parameter
conventions and built-in `Copy`/`Sendable`/`Error` contracts need later checking.
Unknown effect/type relations remain deferred; visibility, full effect rules,
ownership, optional members and runtime dispatch/lowering are also pending.
Nominal ancestry facts describe declared relationships; passing `check` still
establishes only the implemented checks.

Graph traversal is iterative. Lookup caches share results along inheritance
chains and deduplicate diamonds; ancestry queries record visited declarations.
Base/type normalization advances only newly known/error facts, preserving
forward-declaration order and avoiding recursive owning type structures.
