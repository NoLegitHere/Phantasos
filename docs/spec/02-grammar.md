# 2. Grammar [Core]

The standalone [`phantasos.ebnf`](phantasos.ebnf) expands the productions in
this chapter, including every expression precedence level and the lexical
token categories. Keep the file and this chapter in sync. The fragments below
omit concatenation commas and rule-ending semicolons for readability.

`Terms` means one or more statement terminators: `Newline` or `;`. A
terminator is optional right before `}` and at end of file.
Leading and repeated terminators are allowed in files and brace-delimited
lists. Enum members and match arms also accept commas as separators.

## 2.1 Files and items
```
File          = { Import Terms } { Item Terms } EOF
Import        = 'import' Path [ 'as' Ident ]
Path          = Ident { '.' Ident }

Item          = { Attribute } [ Visibility ] { Modifier } ItemKind
Attribute     = '@' ( Ident | Keyword ) [ '(' [ AttrArg { ',' AttrArg } [ ',' ] ] ')' ]   (* e.g. @extern("C") *)
AttrArg       = [ Ident '=' ] Expr
Literal       = IntLit | FloatLit | CharLit | StringLit | 'true' | 'false' | 'none'
Visibility    = 'public' | 'internal' | 'file' | 'private'
Modifier      = 'open' | 'final' | 'abstract' | 'sealed' | 'override'
              | 'static' | 'mut' | 'async' | 'unsafe'
ItemKind      = FunDecl | ClassDecl | StructDecl | EnumDecl | InterfaceDecl
              | ActorDecl | VarDecl | ExternBlock | TypeAlias
              | InitDecl | DeinitDecl                      (* members only *)

FunDecl       = 'fun' Ident [ GenericParams ] '(' [ ParamList ] ')'
                [ ':' Type ] [ Throws ] [ WhereClause ] [ FunBody ]
FunBody       = Block | '=' Expr
Throws        = 'throws' [ '(' Type ')' ]
ParamList     = Param { ',' Param } [ ',' ]
Param         = Ident ':' [ Convention ] Type [ '=' Expr ]
CtorParamList = CtorParam { ',' CtorParam } [ ',' ]
CtorParam     = [ 'let' | 'var' ] Param
Convention    = 'borrow' | 'inout' | 'own'

ClassDecl     = 'class'     TypeHead [ Body ]
StructDecl    = 'struct'    TypeHead [ Body ]
ActorDecl     = 'actor'     TypeHead [ Body ]
InterfaceDecl = 'interface' Ident [ GenericParams ] [ ':' SuperList ] [ WhereClause ] Body
TypeHead      = Ident [ GenericParams ] [ '(' [ CtorParamList ] ')' ] [ ':' SuperList ] [ WhereClause ]
SuperList     = SuperEntry { ',' SuperEntry }
SuperEntry    = Type [ '(' [ ArgList ] ')' ]               (* args only for the superclass *)
Body          = '{' { Item Terms } '}'

EnumDecl      = 'enum' Ident [ GenericParams ] [ ':' SuperList ] [ WhereClause ]
                '{' { ( EnumCase | Item ) ( Terms | ',' ) } '}'
EnumCase      = Ident [ '(' [ CaseField { ',' CaseField } [ ',' ] ] ')' ]   (* items start with a keyword or '@' *)
CaseField     = [ Ident ':' ] Type

InitDecl      = 'init' [ '(' [ ParamList ] ')' [ Throws ] ] Block
DeinitDecl    = 'deinit' Block
VarDecl       = ( 'let' | 'var' | 'const' ) Ident [ ':' Type ] [ '=' Expr ]
ExternBlock   = 'extern' StringLit '{' { ExternFun Terms } '}'
ExternFun     = { Attribute } [ Visibility ] { Modifier } FunDecl   (* e.g. public unsafe fun ... *)
TypeAlias     = 'type' Ident [ GenericParams ] [ ':' Bounds ] [ '=' Type ]
                (* Outside interfaces: '= Type' is required and ': Bounds' is not allowed.
                   Inside an interface body this declares an associated type:
                   `type Item`, `type Item: Equatable`, or with a default `type Index = Int`. *)

GenericParams = '<' GenericParam { ',' GenericParam } [ ',' ] '>'
GenericParam  = Ident [ ':' Bounds ]
Bounds        = Type { '&' Type }
WhereClause   = 'where' Type ':' Bounds { ',' Type ':' Bounds }
```
An `init` with no parameter list is the body of the **primary constructor**,
whose parameters are written after the type name.
Only these primary-constructor parameters may have a `let` or `var` prefix.
Ordinary functions (including methods and extern functions) and secondary
`init(...)` declarations use `ParamList`. For recovery, stage0's parser
accepts misplaced prefixes into the AST; `check` rejects them with `E0207`.

An attribute's argument list must directly follow its name, without whitespace
or comments between the name and `(`. Newlines may separate attributes from
the following attribute or declaration.
Attribute arguments use the ordinary expression grammar, optionally preceded
by a key and `=`. Attribute-specific value restrictions are semantic checks;
stage0 validates the expressions' structure, lexical names and scalar types.

## 2.2 Types
```
Type          = PrefixType { '?' }
PrefixType    = 'shared' PrefixType | 'weak' PrefixType
              | '*' [ 'mut' ] PrefixType                   (* raw pointer; unsafe *)
              | PrimaryType
PrimaryType   = Path [ GenericArgs ] | 'Self'
              | '[' Type ']'                               (* array *)
              | '(' ')'                                    (* Unit *)
              | 'fun' '(' [ Type { ',' Type } [ ',' ] ] ')' [ '->' Type ] [ Throws ]
GenericArgs   = '<' Type { ',' Type } [ ',' ] '>'
```

## 2.3 Statements
```
Block         = '{' { Stmt Terms } '}'
Stmt          = VarDecl
              | 'return' [ Expr ] | 'break' | 'continue' | 'throw' Expr
              | 'while' Cond Block
              | 'for' Ident 'in' Expr Block
              | 'loop' Block
              | 'defer' Block
              | Expr [ AssignOp Expr ]
AssignOp      = '=' | '+=' | '-=' | '*=' | '/=' | '%=' | '&=' | '|=' | '^=' | '<<=' | '>>='
Cond          = 'let' Ident [ ':' Type ] '=' Expr | Expr
```
The optional operand of `return` and the required operand of `throw` must
start before a significant `Newline`. `break` and `continue` have no operand.

## 2.4 Expressions (lowest to highest precedence)
| Level | Operators | Associativity |
|---|---|---|
| 1 | `??` | right |
| 2 | `\|\|` | left |
| 3 | `&&` | left |
| 4 | `== != < <= > >=` | **none** (`a < b < c` is an error) |
| 5 | `is T` | left (postfix-like) |
| 6 | `..` `..<` | none |
| 7 | `+ - &+ &- \| ^` | left |
| 8 | `* / % &* &` | left |
| 9 | `<< >>` | left |
| 10 | `as T`, `as? T` | left (postfix-like) |
| 11 | prefix `- ! ~ try try? await shared` | right |
| 12 | postfix call `()`, member `.x`, `?.x`, index `[]`, unwrap `!` | left |

- Bitwise operators bind tighter than comparison, so `a & mask == 0` means
  `(a & mask) == 0`.
- `as` binds tighter than every binary operator (as in Kotlin), so
  `x as Int64 + 1` means `(x as Int64) + 1`.
- `shared expr` converts an owned value into a `shared` reference (3.4).
- Postfix `!` must directly follow its operand, with no whitespace before it.
- The `?` in `as?` and `try?` must directly follow the keyword, with no
  whitespace or comments between them.

```
Postfix       = Primary { '(' [ ArgList ] ')' | '.' Ident | '?.' Ident | '[' Expr ']' | '!' | GenericArgs }
ArgList       = Arg { ',' Arg } [ ',' ]
Arg           = [ Ident ':' ] Expr
Primary       = Literal | InterpString
              | 'self' | 'super' | 'Self' | Ident
              | '.' Ident                                  (* implicit member, e.g. .Empty *)
              | '(' [ Expr ] ')'                           (* empty parentheses are Unit *)
              | '[' [ Expr { ',' Expr } [ ',' ] ] ']'
              | IfExpr | MatchExpr | 'unsafe' Block
InterpString  = StringHead Expr { StringMiddle Expr } StringTail
IfExpr        = 'if' Cond Block [ 'else' ( IfExpr | Block ) ]
MatchExpr     = 'match' Expr '{' { Arm ( Terms | ',' ) } '}'
Arm           = Pattern [ 'if' Expr ] '->' ( Block | Expr )
```

## 2.5 Patterns
```
Pattern       = AltPattern
AltPattern    = SimplePattern { '|' SimplePattern }
SimplePattern = '_'
              | [ '-' ] ( IntLit | FloatLit ) | CharLit | StringLit | 'true' | 'false' | 'none'
              | Ident                                      (* binds a new name *)
              | '.' Ident [ '(' [ PatList ] ')' ]          (* enum case, type inferred *)
              | Path '.' Ident [ '(' [ PatList ] ')' ]     (* qualified enum case *)
              | Ident '(' [ PatList ] ')'                  (* case/constructor pattern *)
              | 'is' Type [ 'as' Ident ]                   (* type test + binding *)
PatList       = Pattern { ',' Pattern } [ ',' ]
```
A bare identifier is **always** a binding. Enum cases must be written with a
leading dot (`.Empty`) or qualified (`ParseError.Empty`).

## 2.6 Disambiguation rules
1. **Generic arguments in expressions.** After a name or ordinary member
   access, `<` starts a generic argument list when the parser can parse
   `<` Types `>` speculatively
   and the next token is one of `( ) ] } , ; : . ?.`, `Newline`, EOF,
   `StringMiddle`, or `StringTail`.
   Otherwise `<` is a comparison. This is unambiguous because comparison is
   non-associative.
2. **Brace blocks after conditions.** Phantasos has no struct literals and
   no trailing closures in Core, so the `{` after `if`/`while`/`for`/`match`
   heads always starts a block.
3. **Leading-dot chains** are disabled at the top level of match arm bodies
   (see 1.3).
4. **Type arguments after `is`/`as`** are also speculative: `x as Int < y`
   is a comparison, while `x as List<Int>` is a generic type.
   The token after `>` and any optional `?` suffix must end or continue the
   type operand: the closing/separating tokens in rule 1, a binary operator, `is`/`as`,
   `throws` following a function result type, `{` opening a control-flow body,
   or `->` following a match guard. An identifier, literal or
   operand-opening `(` or `.` forces comparison parsing instead. Postfix calls
   or member access on a cast result require parentheses around the expression.
   Thus `f(x as Int < y, z > w)` has two comparison
   arguments. Enclosed parameter, array, generic-argument and throws types
   keep their ordinary type context; function result types retain the outer
   cast/type-test context.
