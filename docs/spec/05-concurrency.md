# 5. Concurrency (full language, v1)

- **`async fun`** functions are compiled into stackless state machines. A
  call to one must be marked with `await` inside another `async` context.
  `fun main` may be `async`.
- **Structured concurrency.**
  - `taskGroup { g -> g.spawn { ... } }` waits for all of its child tasks
    before returning.
  - Cancellation flows from parent to child and is cooperative: a task sees
    it at `await` points.
- **`actor`** is a reference type that serializes access to its mutable
  state.
  - Its methods can be called synchronously from inside the actor.
  - From outside, calls are asynchronous and need `await`.
  - Its `var` state can never be borrowed across an `await`.
- **`Sendable`** is a marker interface that the compiler infers. A value is
  `Sendable` when it can be safely transferred between tasks or actors.
  - Sendable: value types whose fields are all `Sendable`, deeply immutable
    classes, actors, and `shared T` where `T: Sendable`.
  - Passing a non-`Sendable` value across an isolation boundary is a
    compile-time error, unless the value is **moved** (uniquely owned
    transfer).
- **Runtime.** The executor is a work-stealing pool with one worker per
  core. Its building blocks are:
  - Linux: `futex` and `clone`.
  - Windows: `WaitOnAddress`/`WakeByAddress*` and `CreateThread`.
  - Reference counts become atomic only for values that are shared across
    tasks.
- `std.sync` provides `Mutex<T>`, `Atomic<T>`, `Channel<T>`, and `Once`.
