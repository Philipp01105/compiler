# DMM Formal Language

This document defines DMM's lexical and syntactic grammar using Extended Backus-Naur Form (EBNF). `LANGUAGE_SPEC.md`
defines the accompanying semantic rules and implementation limits.

## Notation

```ebnf
x | y        (* alternative *)
[x]          (* optional *)
{x}          (* zero or more repetitions *)
(x)          (* grouping *)
"text"       (* terminal text *)
```

Whitespace separates tokens and is otherwise insignificant. A line comment begins with `//` and continues through the
end of the line. A non-nesting block comment begins with `/*` and ends at the next `*/`, and may span lines.

## Lexical grammar

```ebnf
letter          = "A" … "Z" | "a" … "z" | "_" ;
digit           = "0" … "9" ;
identifier      = letter, { letter | digit } ;

integer         = digit, { digit } ;
exponent        = ("e" | "E"), ["+" | "-"], digit, { digit } ;
floating        = digit, { digit }, ".", digit, { digit }, [exponent]
                | digit, { digit }, exponent ;

escape          = "\\", ("n" | "t" | "r" | "0" | "\\" | "'" | '"') ;
character       = "'", (escape | character-byte), "'" ;
string          = '"', { escape | string-byte }, '"' ;

keyword         = "func" | "var" | "return" | "for" | "if" | "else"
                | "while" | "const" | "break" | "continue"
                | "struct" | "enum" | "import" | "static" | "reserve"
                | "free" | "interface" | "match" | "package" | "pub"
                | "sizeof" | "alignof" | "slice" | "case" | "typeof"
                | "destructor" | "defer" | "mut" ;

primitive-type  = "int" | "char" | "byte" | "bit"
                | "float" | "double" | "string" | "void"
                | "i8" | "u8" | "i16" | "u16" | "i32" | "u32"
                | "i64" | "u64" | "isize" | "usize" ;
```

`character-byte` excludes quote, backslash, and line terminators. `string-byte` excludes double quote, backslash, and
line terminators. A leading sign is parsed as a unary operator rather than as part of a numeric token.

## Program grammar

```ebnf
program         = "package", identifier, ";", { top-level-declaration }, end-of-file ;

top-level-declaration
                = import-declaration
                | ["pub"], (function-declaration | struct-declaration
                | enum-declaration | constant-declaration | package-variable
                | interface-declaration) ;
package-variable = "var", identifier, [":", type], ["=", expression], ";" ;

import-declaration
                = "import", (import-entry | "(", import-entry, {import-entry}, ")"), ";" ;
import-entry    = [identifier], string ;
qualified-name  = identifier, [".", identifier] ;

function-declaration
                = "func", identifier, [generic-parameters], "(", [parameter-list], ")",
                  "->", return-type, block ;
parameter-list  = parameter, { ",", parameter } ;
parameter       = identifier, ":", type ;
return-type     = type ;
type            = ["&", ["mut"]], {"*"},
                  (function-type | primitive-type | qualified-name, [type-arguments] | "(", type, ")"),
                  {"[", [integer | identifier], "]"} ;
function-type   = "func", [generic-parameters], "(", [type-list], ")", "->", type ;
type-arguments  = "<", type, {",", type}, ">" ;
generic-parameters = "<", generic-parameter, {",", generic-parameter}, ">" ;
generic-parameter = identifier, [":", qualified-name, {"+", qualified-name}] ;
constant-declaration
                = "const", identifier, [":", type], "=", expression, ";" ;
```

Struct and enum members use the same function and variable declaration forms accepted by their parser contexts:

```ebnf
struct-declaration
                = "struct", identifier, [generic-parameters], "{", { struct-member }, "}" ;
struct-member   = ["pub"], (field-declaration | ["static"], function-declaration)
                | destructor-declaration ;
destructor-declaration = "destructor", block ;
field-declaration
                = "var", identifier, ":", type, ";" ;

enum-declaration
                = "enum", identifier, [generic-parameters], ["(", enum-field-list, ")"],
                  "{", enum-value, { ",", enum-value }, [","], "}" ;
enum-field-list = enum-field, { ",", enum-field } ;
enum-field      = ["pub"], identifier, ":", type ;
enum-value      = ["pub"], identifier, ["(", [enum-argument-list | type-list], ")"] ;
type-list       = type, {",", type} ;
interface-declaration = "interface", identifier, "{", {interface-method}, "}" ;
interface-method = ["pub"], "func", identifier, "(", [parameter-list], ")", "->", type, ";" ;
enum-argument-list
                = enum-argument, { ",", enum-argument } ;
enum-argument   = integer | floating | character | string
                | "true" | "false" ;
```

`resource` is not a keyword or declaration modifier. It is an ordinary identifier, and the former
`resource struct` spelling is rejected. Resource semantics are derived from the normal struct declaration's
destructor and concrete field types.

Enum field and member names are unique. Each value supplies exactly one compatible argument for every declared field.
Without header fields, variant parentheses contain payload types rather than constant arguments. Generic nominal names
followed by `.variant` accept type arguments in constructor expressions.

## Statements

```ebnf
block           = "{", { statement }, "}" ;

statement       = block
                | variable-declaration
                | assignment-statement
                | call-statement
                | if-statement
                | while-statement
                | for-statement
                | return-statement
                | break-statement
                | continue-statement
                | match-statement
                | defer-statement
                | constant-declaration ;

variable-declaration
                = "var", identifier, [":", type], ["=", expression], ";" ;
variable-declaration-without-semicolon
                = "var", identifier, [":", type], ["=", expression] ;

assignment-statement
                = lvalue, assignment-operator, expression, ";"
                | lvalue, ("++" | "--"), ";" ;
assignment-operator
                = "=" | "+=" | "-=" | "*=" | "/=" ;
lvalue          = expression ; (* semantic analysis requires mutable storage *)

call-statement  = call-expression, ";" ;
if-statement    = "if", "(", expression, ")", statement,
                  ["else", (if-statement | statement)] ;
while-statement = "while", "(", expression, ")", statement ;
for-statement   = "for", "(", [for-initializer], ";", [expression], ";",
                  [for-update], ")", statement ;
for-initializer = variable-declaration-without-semicolon
                | lvalue, "=", expression ;
for-update      = lvalue, assignment-operator, expression
                | lvalue, ("++" | "--") ;

return-statement = "return", [expression], ";" ;
break-statement  = "break", ";" ;
continue-statement = "continue", ";" ;
match-statement = "match", "(", (expression | type), ")", "{", {match-arm}, "}" ;
match-arm       = (identifier, ["(", [identifier, {",", identifier}], ")"] | "_"),
                  "=>", statement
                | "case", (type | "_"), "->", statement ;
defer-statement = "defer", (call-expression, ";" | "func", "(", ")", block) ;
```

## Expressions

The grammar encodes precedence from lowest to highest. Binary operators at each level associate left-to-right; unary
operators associate right-to-left.

```ebnf
expression      = logical-or ;
logical-or      = logical-and, { "||", logical-and } ;
logical-and     = comparison, { "&&", comparison } ;
comparison      = additive, { comparison-operator, additive } ;
comparison-operator
                = "==" | "!=" | "<" | "<=" | ">" | ">=" ;
additive        = multiplicative, { ("+" | "-"), multiplicative } ;
multiplicative  = unary, { ("*" | "/" | "%"), unary } ;
unary           = ("!" | "-" | "&" | "*"), unary | postfix-expression ;
postfix-expression = primary, {postfix} ;

primary         = integer | floating | character | string
                | "true" | "false"
                | identifier
                | type-metadata
                | ("reserve" | "sizeof" | "alignof"), "(", type, ")"
                | "slice", "(", expression, ",", expression, ")"
                | array-literal
                | "free"
                | "(", expression, ")" ;

array-literal   = "[", expression, {",", expression},
                  [";", expression], "]" ;

identifier-expression
                = identifier, { postfix } ;
postfix         = "(", [argument-list], ")"
                | type-arguments, ["(", [argument-list], ")"]
                | "[", expression, "]"
                | ".", identifier
                | ".", "(", type, ")" ;
argument-list   = expression, { ",", expression } ;
call-expression = identifier-expression ;
type-metadata   = type, ".", ("name" | "size" | "align") ;
```

`expression.type` is a member expression denoting compile-time static type metadata. It may be followed by `.name`,
`.size`, or `.align`, or used as a type-match scrutinee. Type matches use `case Type -> statement` and
`case _ -> statement`; enum matches use variant patterns with `=>`. See `LANGUAGE_SPEC.md` for specialization and
unevaluated-expression rules. `typeof` is currently reserved by the lexer; use `.type` for static type access.

Array literals require an expected fixed-array or slice type. Their prefix is nonempty. In the repetition form
`[a,b,c;N]`, semantic analysis requires `N` to be a positive compile-time integer constant and produces exactly `N`
elements by cycling the prefix. An ordinary fixed-array literal must contain exactly the target length.

## Context-sensitive validity

The grammar describes structure only. A valid DMM program must also satisfy the rules in `LANGUAGE_SPEC.md`, including
declaration-before-use and scope rules, type compatibility, valid return paths, argument matching, loop-only `break`/
`continue`, object-size limits, and array-bounds requirements.
