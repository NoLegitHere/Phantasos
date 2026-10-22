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

## 3.3 Mutability [Core]
- `let` creates an immutable binding and `var` a mutable one. Fields follow
  the same rule.
- A method can only mutate `self` if it is declared `mut fun`. Calling it
  requires a mutable place: a `var` binding, an `inout` parameter, or `self`
  inside another `mut fun`.
- Immutability is **deep for value types**. For class references it is
  shallow: a `let` binding holding a class reference cannot be reassigned,
  but the object can be mutated through its `mut` methods if its fields are
  `var`. A class whose fields are all `let` is **deeply immutable** and is
  automatically `Sendable`.

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
