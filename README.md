# Phantasos

Phantasos is a programming language that compiles to native code. It is in
early development: the language is specified in draft form, and the bootstrap
compiler runs a growing subset of it on Linux x86_64.

The design goals, from [ADR 0002](docs/adr/0002-foundational-decisions.md),
are:

- Object-oriented, immutable by default, with no `null`: single class
  inheritance, `final` by default, interfaces with default methods and sealed
  hierarchies.
- Memory safety without a garbage collector: single ownership with moves,
  borrows checked at compile time without lifetime annotations, opt-in
  `shared` (reference counted) and `weak` references, and deterministic
  `deinit`.
- Typed errors with `throws(E)`, returned as values rather than unwound.
- `async`/`await`, actors and `Sendable` checking.
- A toolchain of its own: x86 backends, object writers and linkers for PE
  and ELF, with no LLVM and no C library. Planned targets are Windows x64,
  Linux x86_64 and Linux i386.

Most of this is not implemented yet. The [roadmap](docs/roadmap.md) tracks
what is.

## Status

Bootstrap phase P1. The stage0 compiler in [`bootstrap/stage0`](bootstrap/stage0)
is written in C++23 and builds and passes its tests on Windows x64 and Linux
x86_64. It has:

- A lexer and parser for Phantasos-Core, with error recovery and stable
  diagnostic codes.
- `check`: name resolution, type checking (including generics, interfaces
  and inheritance), flow checks (returns, initialization, mutability) and
  ownership checks (use after move, moves out of borrows, exclusive `inout`).
  Parts of the type system are still incomplete, so a program that passes
  `check` is not yet guaranteed to be valid.
- Native code for a subset of Core: integers, `Bool`, `String` literals,
  structs, tuples, methods, function values, closures, and generic functions
  and structs with interface bounds. `build` turns such a program into a
  static Linux x86_64 executable with a small runtime of its own.

Not done yet: Windows executables, arrays, optionals, classes, enums, error
handling, destructors, a standard library, and package/workspace builds.

The language is specified in [`docs/spec`](docs/spec), with a standalone
[Core EBNF grammar](docs/spec/phantasos.ebnf).

## What runs today

`build <file.phs> [-o <out>]` compiles a program to a Linux x86_64
executable. Its exit status is the value `main` returns. Integer overflow and
division by zero stop the program with a panic message and exit status 101.
`print` writes a `String`, `Int`, `UInt` or `Bool` followed by a newline.
Anything outside the supported subset gets an `E0600` diagnostic; spec
[3.25](docs/spec/03-types-and-semantics.md) lists the subset.

```phantasos
fun main() {
    print("Hello, world!")
}
```

Structs, methods and tuples
([`run/vectors.phs`](tests/stage0/run/vectors.phs),
[`run/bank_account.phs`](tests/stage0/run/bank_account.phs),
[`run/divmod.phs`](tests/stage0/run/divmod.phs)):

```phantasos
struct Vec(x: Int, y: Int) {
    fun plus(other: Vec): Vec = Vec(x: x + other.x, y: y + other.y)
}

fun divmod(a: Int, b: Int): (Int, Int) = (a / b, a % b)

fun main(): Int {
    let v = Vec(x: 3, y: 4).plus(Vec(x: 1, y: 1))
    let (q, r) = divmod(v.x * v.y, 7)
    print(q)
    return r
}
```

Functions are values: they can be passed, returned, stored in fields and
called through a variable
([`run/higher_order.phs`](tests/stage0/run/higher_order.phs),
[`run/calculator.phs`](tests/stage0/run/calculator.phs),
[`run/sorting.phs`](tests/stage0/run/sorting.phs)):

```phantasos
fun square(x: Int): Int = x * x
fun plus(a: Int, b: Int): Int = a + b

fun fold(from: Int, to: Int, initial: Int, f: fun(Int) -> Int, combine: fun(Int, Int) -> Int): Int {
    var total = initial
    var i = from
    while i < to {
        total = combine(total, f(i))
        i += 1
    }
    return total
}

fun main(): Int = fold(1, 5, 0, square, plus)  // 1 + 4 + 9 + 16 = 30
```

Brace closures capture copies of the values they use, and can outlive the
call that made them
([`run/adders.phs`](tests/stage0/run/adders.phs),
[`run/scores.phs`](tests/stage0/run/scores.phs),
[`run/rules.phs`](tests/stage0/run/rules.phs)):

```phantasos
fun makeAdder(n: Int): fun(Int) -> Int = { x -> x + n }

fun compose(f: fun(Int) -> Int, g: fun(Int) -> Int): fun(Int) -> Int = { x -> g(f(x)) }

fun main(): Int {
    let add5 = makeAdder(5)
    let double = { x: Int -> x * 2 }
    return compose(double, add5)(10)  // 25
}
```

Values that are not `Copy`, such as `String` and structs that hold one, move
when they are passed to an `own` parameter, stored or returned. `check`
reports a use after a move (`E0504`), a move out of something only borrowed
(`E0505`) and an `inout` argument or `mut fun` receiver that overlaps another
argument of the same call (`E0506`); `.clone()` makes a copy that can move
instead
([`run/inventory.phs`](tests/stage0/run/inventory.phs),
[`run/greetings.phs`](tests/stage0/run/greetings.phs),
[`run/clone.phs`](tests/stage0/run/clone.phs)):

```phantasos
struct Item(name: String, var count: Int) {
    mut fun add(n: Int) { count += n }
}

fun stock(name: own String, count: Int): Item = Item(name, count)

fun main(): Int {
    let label = "bolts"
    var bolts = stock(label.clone(), 10)  // `stock(label, 10)` would move label
    print(label)
    let moved = bolts                     // bolts cannot be used after this
    var spare = moved.clone()
    spare.add(moved.count)
    return spare.count                    // 20
}
```

Generic functions and structs are compiled once per set of type arguments
used, and calls through an interface bound go directly to the struct's method
([`run/leaderboard.phs`](tests/stage0/run/leaderboard.phs),
[`run/pipeline.phs`](tests/stage0/run/pipeline.phs),
[`run/interfaces.phs`](tests/stage0/run/interfaces.phs)):

```phantasos
interface Ranked {
    fun score(): Int
    fun beats(other: Self): Bool = score() > other.score()
}

struct Player(points: Int): Ranked {
    fun score(): Int = points
}

struct Pair<A, B>(first: A, second: B) {
    fun swap(): Pair<B, A> = Pair(first: second, second: first)
}

fun best<T: Ranked>(a: T, b: T): T {
    if b.beats(a) { return b }
    return a
}

fun twice<T>(f: fun(T) -> T): fun(T) -> T = { x -> f(f(x)) }

fun main(): Int {
    let winner = best(Player(points: 7), Player(points: 9))
    let p = Pair(first: true, second: winner.score()).swap()
    return twice({ x: Int -> x + 1 })(p.first)  // 11
}
```

## Repository layout

| Path | Purpose |
|---|---|
| `bootstrap/stage0/` | The C++23 bootstrap compiler (`phantasos-stage0`) and its C++ tests |
| `docs/spec/` | Language and ABI specification |
| `docs/adr/` | Architecture decision records |
| `docs/roadmap.md` | Bootstrap milestones |
| `tests/` | Golden tests, fuzzers and stress tests |
| `tools/` | Developer tools such as `license-check` |

## Building stage0

You need clang++ 18 or later with a C++23 standard library that passes the
CMake feature probe, CMake 3.28 or later, and Ninja. CI uses Clang 19 with
libstdc++ 14 on Linux and Clang with the MSVC STL on Windows.

On Ubuntu 24.04:

```sh
sudo apt-get install clang-19 g++-14 ninja-build
cmake -S bootstrap/stage0 -B build/stage0 -G Ninja -DCMAKE_CXX_COMPILER=clang++-19 -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS=--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/14
cmake --build build/stage0
```

On Windows, or with a compatible toolchain already set up:

```sh
cmake -S bootstrap/stage0 -B build/stage0 -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug
cmake --build build/stage0
```

Run the tests:

```sh
ctest --test-dir build/stage0 --output-on-failure                    # C++ tests
python tests/run_golden.py --stage0 build/stage0/phantasos-stage0      # golden tests
python tests/test_frontend.py --stage0 build/stage0/phantasos-stage0   # nesting and input limits
python tests/fuzz_frontend.py --stage0 build/stage0/phantasos-stage0   # fuzzing; add --command check|gate|mir|asm|build
python tools/license-check/license_check.py                            # license headers
```

[`tests/stress/profile_hierarchy.py`](tests/stress/profile_hierarchy.py)
times and profiles `check` on large generated class hierarchies; CI runs it
with memory and growth budgets. See [CONTRIBUTING.md](CONTRIBUTING.md) for
more on the tests.

## Trying it

```sh
build/stage0/phantasos-stage0 help                                     # list the commands
build/stage0/phantasos-stage0 parse tests/stage0/parser/showcase.phs   # print the syntax tree
build/stage0/phantasos-stage0 check tests/stage0/run/inventory.phs     # type, flow and ownership checks
build/stage0/phantasos-stage0 mir   tests/stage0/mir/fib.phs           # print the MIR
build/stage0/phantasos-stage0 asm   tests/stage0/asm/fib.phs           # print the x86_64 code
build/stage0/phantasos-stage0 build tests/stage0/run/hello.phs -o hello
./hello                                                                # prints Hello, world!
```

Every program in [`tests/stage0/run`](tests/stage0/run) builds the same way;
its expected output and exit status sit next to it.

## License

Apache License 2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE). Every source
file must start with the project license header, and CI enforces this with
`tools/license-check`.
