# DMM Formal Language

This document defines DMM's lexical and syntactic grammar using Extended Backus-Naur Form (EBNF). `LANGUAGE_SPEC.md` defines the accompanying semantic rules and implementation limits.

## Notation

```ebnf
x | y        (* alternative *)
[x]          (* optional *)
{x}          (* zero or more repetitions *)
(x)          (* grouping *)
"text"       (* terminal text *)
```

Whitespace separates tokens and is otherwise insignificant. A line comment begins with `//` and continues through the end of the line.

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
                | "while" | "print" | "println" | "break" | "continue"
                | "struct" | "enum" | "import" | "static" | "reserve"
                | "free" | "gc" ;

primitive-type  = "int" | "char" | "byte" | "bit"
                | "float" | "double" | "string" | "void" ;
```

`character-byte` excludes quote, backslash, and line terminators. `string-byte` excludes double quote, backslash, and line terminators. A leading sign is parsed as a unary operator rather than as part of a numeric token.

## Program grammar

```ebnf
program         = { top-level-declaration }, end-of-file ;

top-level-declaration
                = import-declaration
                | function-declaration
                | struct-declaration
                | enum-declaration ;

import-declaration
                = "#", "import", (string | "<", import-path, ">") ;
import-path     = import-part, { import-part } ;
import-part     = identifier | integer | "." | "/" | "-" ;

function-declaration
                = "func", identifier, "(", [parameter-list], ")",
                  "->", return-type, block ;
parameter-list  = parameter, { ",", parameter } ;
parameter       = identifier, ["[", "]"], ":", parameter-type ;
parameter-type  = ["*"], type ;
return-type     = type ;
type            = primitive-type | identifier ;
```

Struct and enum members use the same function and variable declaration forms accepted by their parser contexts:

```ebnf
struct-declaration
                = "struct", identifier, "{", { struct-member }, "}" ;
struct-member   = field-declaration | ["static"], function-declaration ;
field-declaration
                = "var", identifier, ":", type, ["[", integer, "]"], ";" ;

enum-declaration
                = "enum", identifier, "{", enum-body, "}" ;
```

The detailed enum value constraints are semantic: field names and value records must agree with the declared enum layout.

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
                | print-statement ;

variable-declaration
                = ["@", "gc"], "var", variable-shape, identifier,
                  [":", variable-type], ["=", expression], ";" ;
variable-shape  = ["[", integer, "]"] ;
variable-type   = ["*"], type ;

assignment-statement
                = lvalue, assignment-operator, expression, ";"
                | lvalue, ("++" | "--"), ";" ;
assignment-operator
                = "=" | "+=" | "-=" | "*=" | "/=" ;
lvalue          = identifier, { "[", expression, "]" | ".", identifier | "*" } ;

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
print-statement  = ("print" | "println"), "(", [expression], ")", ";" ;
```

## Expressions

The grammar encodes precedence from lowest to highest. Binary operators at each level associate left-to-right; unary operators associate right-to-left.

```ebnf
expression      = logical-or ;
logical-or      = logical-and, { "||", logical-and } ;
logical-and     = comparison, { "&&", comparison } ;
comparison      = additive, { comparison-operator, additive } ;
comparison-operator
                = "==" | "!=" | "<" | "<=" | ">" | ">=" ;
additive        = multiplicative, { ("+" | "-"), multiplicative } ;
multiplicative  = unary, { ("*" | "/" | "%"), unary } ;
unary           = ("!" | "-" | "&" | "*"), unary | primary ;

primary         = integer | floating | character | string
                | "true" | "false"
                | identifier-expression
                | "(", expression, ")" ;

identifier-expression
                = identifier, { postfix } ;
postfix         = "(", [argument-list], ")"
                | "[", expression, "]"
                | ".", identifier, ["(", [argument-list], ")"] ;
argument-list   = expression, { ",", expression } ;
call-expression = identifier-expression ;
```

## Context-sensitive validity

The grammar describes structure only. A valid DMM program must also satisfy the rules in `LANGUAGE_SPEC.md`, including declaration-before-use and scope rules, type compatibility, valid return paths, argument matching, loop-only `break`/`continue`, object-size limits, and array-bounds requirements.
