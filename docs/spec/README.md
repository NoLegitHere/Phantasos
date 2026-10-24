# Phantasos Language Specification (draft)

This is the normative description of the Phantasos language. Until v1.0 it is
a living draft. Sections marked **[Core]** describe Phantasos-Core, the subset
that the stage0 bootstrap compiler implements and that the self-hosted
compiler is written in. Unmarked sections belong to the full language.

| Chapter | Contents |
|---|---|
| [01-lexical.md](01-lexical.md) | Source text, tokens, newlines, literals |
| [02-grammar.md](02-grammar.md) | Syntax (EBNF), operator precedence, disambiguation rules |
| [03-types-and-semantics.md](03-types-and-semantics.md) | Type kinds, ownership, construction/destruction, errors, visibility |
| [04-packages-and-modules.md](04-packages-and-modules.md) | Packages, modules, auto-discovery, workspaces, `Phantasos.toml` |
| [05-concurrency.md](05-concurrency.md) | `async`/`await`, tasks, actors, `Sendable` |
| [abi.md](abi.md) | Object layout, vtables, ARC header, calling conventions, mangling |

## Notation

The grammar uses EBNF: `{ x }` means zero or more, `[ x ]` means optional,
`|` separates alternatives, and `'tok'` is a literal token.
