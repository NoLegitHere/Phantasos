# 1. Lexical structure [Core]

## 1.1 Source text
- Source files use the `.phs` extension and are encoded in UTF-8. A leading
  BOM is ignored. Invalid UTF-8 anywhere in a file, including inside
  comments and literals, is an error (`E0011`). Invalid UTF-8 means truncated
  or overlong sequences, encoded surrogates, values above U+10FFFF, or stray
  continuation bytes.
- `CRLF` and lone `CR` are both treated as a single line terminator.
- Non-ASCII characters are allowed only in comments, string literals, and
  character literals. Unicode identifiers (UAX #31 XID) are planned for the
  full language.

## 1.2 Whitespace and comments
- Space, horizontal tab, and form feed separate tokens.
- `// ...` is a line comment and `/// ...` is a doc comment, which attaches
  to the following item.
- `/* ... */` is a block comment, and block comments **nest**. An
  unterminated block comment is an error.

## 1.3 Newlines
Newlines terminate statements, items, and match arms. The lexer emits a
`Newline` token (consecutive blank lines collapse into one) **only when the
innermost open delimiter is `{` or there is none**, so the lexer drops
newlines inside `(...)` and `[...]`. Inside generic `<...>` parameter and
argument lists, the **parser** ignores them, because only the parser can
tell a generic list from a comparison.

The parser also skips newlines:
- after any binary operator, `,`, `=`, `->`, `:`, `.`, `?.`, and after an
  opening delimiter (so a line ending in an operator continues the
  expression);
- before any required type, e.g. after `:`, `=`, `->`, `&`, `is`, or `as`;
- before `.` and `?.` (leading-dot member chains), **except** directly at
  the top level of a `match` arm body, where a line starting with `.`
  begins a new arm;
- before `else`, so `}` followed by `else` on the next line works.

A line starting with a binary operator does **not** continue the previous
line. For example, `a` followed by a new line `- b` is two statements.

`;` may be used as an explicit terminator.

## 1.4 Identifiers and keywords
```
Ident = (Letter | '_') { Letter | Digit | '_' }      ; Letter = [A-Za-z], Digit = [0-9]
```
A lone `_` is the wildcard token, not an identifier.

Reserved keywords:
```
abstract actor as async await borrow break class const continue defer deinit
else enum extern false file final for fun if import in init inout interface
internal is let loop match mut none open override own private public return
sealed self Self shared static struct super throw throws true try type unsafe
var weak where while
```
Reserved for future use, and lexed as keywords: `comptime macro yield spawn`.

## 1.5 Literals
- **Integer**: `123`, `1_000`, `0xFF`, `0o17`, `0b1010`. Underscores separate
  digits but cannot appear first after a prefix. The value must fit in 64
  bits. Integer literals are untyped until type inference gives them a type
  (default `Int`).
- **Float**: `1.5`, `1e9`, `2.5e-3`. The `.` must have a digit on both sides
  (`1.` and `.5` are not floats), so `1..2` lexes as `1` `..` `2`. Defaults to
  `Float64`. The value is rounded to the nearest `Float64`; a literal whose
  value overflows to infinity, or a non-zero literal that underflows to zero,
  is an error (`E0005`). Subnormal results are allowed.
- **Character**: `'a'`, `'\n'`, `'\u{1F600}'`. Its value is one Unicode
  scalar value, of type `Char`.
- **String**: `"text"`. Escapes are `\n \r \t \0 \\ \" \' \u{H..H}`
  (1 to 6 hex digits).
- **String interpolation**: `"x = \(x + 1)!"`. The lexer produces
  `StringHead` (`"x = `), the expression tokens, then `StringTail` (`!"`).
  For more than one interpolation, the segments between them are
  `StringMiddle`. Interpolations may nest.
- Multi-line `"""` strings are reserved for the full language.

## 1.6 Operators and punctuation
```
+  -  *  /  %  &+ &- &*         arithmetic (&-forms wrap; plain forms trap on overflow)
&  |  ^  ~  << >>               bitwise
== != < <= > >=                 comparison
&& || !                         logical
=  += -= *= /= %= &= |= ^= <<= >>=   assignment (statements only)
?  ?. ?? !                      optionals (postfix ! = unwrap-or-panic)
.  .. ..<  ,  :  ;  ->  =>  @  (  )  [  ]  {  }  _
```
Tokens are formed by maximal munch. Inside generic argument lists, the
parser splits `>>`, `>=`, and `>>=` into their parts.
