# ABI and object model (draft)

The **Phantasos-native ABI** is unstable and may change between toolchain
versions; packages are always built with a single toolchain. Only
`extern "C"` and `@export("C")` declarations follow the stable **platform C
ABI**.

## A.1 Targets and primitive layout
| Target | Pointer / `Int` | C calling convention | Object format | Executable |
|---|---|---|---|---|
| `windows-x64` | 8 | Microsoft x64 | COFF | PE32+ |
| `linux-x86_64` | 8 | System V AMD64 | ELF64 | ELF64 (static, `ET_EXEC`/`ET_DYN` PIE later) |
| `linux-i386` | 4 | System V i386 (cdecl) | ELF32 | ELF32 |

- Integer and float types have their natural size and alignment.
- Exception: in `@repr("C")` types on i386, `Int64` and `Float64` are aligned
  to 4 bytes, matching the i386 C ABI. Phantasos-layout types always align
  them to 8.
- `Bool` is 1 byte (0 or 1), and `Char` is 4 bytes.

## A.2 Struct layout
- By default, fields are **reordered by decreasing alignment** (stable
  within equal alignment) to minimize padding.
- `@repr("C")` keeps declaration order with C padding rules and is required
  for FFI.
- Zero-sized structs take up 0 bytes inside other aggregates.

## A.3 Class objects
```
offset 0          vptr         -> vtable
offset P          strong: UInt32   (strong reference count; 1 when uniquely owned)
offset P+4        weak:   UInt32   (weak count + 1 while strong > 0)
offset align(P+8) fields of the root class, then fields of each subclass in order
```
(P = pointer size.) Superclass fields come before subclass fields, so a
pointer to a subclass object is also a valid pointer to its superclass.

- **vtable**:
  - `[0]` is the type descriptor.
  - `[1]` is `destroy(self)`, which runs the `deinit` chain, destroys fields,
    and frees memory (or only releases the strong portion if weak refs
    exist).
  - `[2..]` are virtual methods: superclass slots first, then new `open`
    methods in declaration order. An override reuses its parent's slot.
- **Type descriptor**: `{ size, align, depth, ancestors: *[*TypeDesc] (ancestors[depth] == self), name: *UInt8, itables: *[(*IfaceDesc, *ITable)], itable_count }`.
  This makes the `is` check O(1): `obj.desc.depth >= T.depth && obj.desc.ancestors[T.depth] == T`.

## A.4 Interfaces
- An **existential** value (a value whose static type is an interface) is a
  fat pointer `{ data: *Object, itable: *ITable }`.
- An itable has the same shape as the vtable method area for that
  interface's requirements. Default methods fill any slot the type does not
  implement itself.
- Generic code with interface bounds is monomorphized and needs no itables.

## A.5 Enums and optionals
- `enum`: the tag is the smallest unsigned integer that fits the number of
  cases, followed by a payload union aligned to the largest payload
  alignment.
- **Niche optimization**: `T?` for a class reference, `shared T`, `weak T`,
  or a non-null pointer is a single pointer, with `none == 0`.

## A.6 Calling convention (native)
Phantasos-native calls use the platform C convention, plus hidden
parameters in this order:

1. `ret*`: an indirect return slot, when the platform C ABI would use sret
   or the function throws.
2. `err*`: an error slot (only for throwing functions).
3. `self`: the receiver.
4. The explicit parameters in source order.

- **Throwing functions** return a status `Bool` in `al` (0 = success,
  1 = error). The value is written to `*ret` and the error to `*err`.
- `borrow` parameters of aggregates larger than 2 pointers are passed by
  address. `inout` is always passed by address.
- `own` parameters transfer ownership: the callee destroys them.

## A.7 Symbol mangling
```
symbol   = '_P' [ '$' ] path [ generics ] [ labels ]
path     = { len ident }            ; package, module components, type, member
generics = 'G' { type } 'E'
labels   = 'L' { len ident } 'E'    ; only for init overloads
type     = 'i1'|'i2'|'i4'|'i8'|'u1'|'u2'|'u4'|'u8'|'f4'|'f8'|'b'|'c'|'s'
         | 'N' path [ generics ]    ; nominal
         | 'O' type                 ; optional
         | 'A' type                 ; array
         | 'S' type | 'W' type      ; shared / weak
```
- Example: `geometry.shapes.Circle.area` is mangled as
  `_P8geometry6shapes6Circle4area`.
- The `_P$` prefix is reserved for privileged internal symbols, which are
  always emitted with hidden visibility.

## A.8 Program entry
- The runtime provides the process entry point: `_start` on Linux, and
  `__phantasos_entry` on Windows (set as `AddressOfEntryPoint`).
- The entry point initializes TLS, the allocator, and argv/env, then calls
  the package's mangled `main`, and finally calls `exit` with the result.
- No C runtime is linked.
- Stage0 emits a minimal Linux entry instead: `_start` calls `main` and
  passes its result (0 for a `Unit` main) to the `exit_group` system call,
  and `__phantasos_trap` writes a panic message with `write` and exits with
  status 101. The executable is `ET_EXEC` at `0x400000` with a read-only,
  executable `PT_LOAD` segment for code and a separate, page-aligned
  read-only, non-executable one for `.rodata` (string bytes such as the panic
  messages). Identical byte strings within an object share one copy.
