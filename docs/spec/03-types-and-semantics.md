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
chapter 2 before running the name resolver below. Typing, visibility,
return-type, ownership and effect analysis remain on the [roadmap](../roadmap.md).

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
  Initialization safety, inheritance validity and static/instance member
  access restrictions are checked by later passes.

Stage0 retains immutable scope, declaration, overload-set and reference
records separately from the AST. References are explicitly resolved,
unresolved (with a diagnostic), or deferred. It resolves directly declared
nested types and type-qualified values. Receiver-dependent members, implicit
`.Case` names, alias-qualified names and dependent generic members such as
`T.Element` are deferred until type information is available. Inherited
lookup is also deferred, including an unqualified name that a base could
provide before an outer scope is searched. A type's own generic parameters
take precedence over that inherited lookup. Lexical lookup alone cannot
select inherited members or check visibility. Alias cycles and initialization
cycles remain later checks.

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

Scalar arithmetic (`+ - * /`) requires operands of the same numeric type.
Remainder, wrapping arithmetic, shifts and bitwise operators require the same
integer type. Unary `-` requires signed numeric operands, and `~` requires an
integer. `!`, `&&`, `||`, ordinary `if`/`while` conditions and match guards
require `Bool`. Scalar equality accepts matching built-in types; ordering
accepts matching numeric types or `Char`. There are no implicit numeric
conversions between typed operands. Compound assignment follows the same
numeric/integer domains and requires the target's type.

Scalar variable annotations, parameter defaults, assignments and explicit return
values must match their expected type. A block-bodied function without a return
annotation returns `Unit`; expression-bodied return inference remains pending.
An explicit bare `return` produces `Unit`. Initializers and destructors also
return `Unit`. This does not establish that every path returns a value.

After successful validation and name resolution, stage0 checks these scalar
rules and retains expression/binding types. `E0400` reports mismatches,
`E0401` invalid operator domains, and `E0402` out-of-range literals. A type error
does not alter the syntax tree, and dependent expressions avoid cascading
diagnostics. Parsing alone performs none of these checks.

The scalar API explicitly returns `Deferred` for calls, callable values,
composite/nominal/generic types, aliases, receiver-dependent members, optional
bindings, casts, effect operators and block/branch results. It traverses their
children to check supported scalar subexpressions. Forward variable references
can infer scalar types. Numeric literals in deferred contextual positions keep
deferred types instead of defaulting prematurely (for example, an unchecked
call argument could require `UInt64` rather than `Int`). Cyclic inference
without an established type remains
deferred. Function signatures/calls, conversions, initialization cycles,
all-path returns, mutability, ownership and effects still need later passes.
Successful `check` therefore continues to mean only that implemented checks
found no errors, not that the program is valid or executable.
