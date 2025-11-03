# 📐 Formal Grammar Specification - Compiler Version 4.6.0

**Author:** Philipp01105  
**Date:** 2025-11-03  
**Version:** 4.6.0 (with break/continue)  
**Notation:** Extended Backus-Naur Form (EBNF)

---

## 📑 Table of Contents

1. [Notation Conventions](#1-notation-conventions)
2. [Lexical Grammar](#2-lexical-grammar)
3. [Syntactic Grammar](#3-syntactic-grammar)
4. [Operator Precedence](#4-operator-precedence)
5. [Type System Grammar](#5-type-system-grammar)
6. [Examples](#6-examples)

---

## 1. Notation Conventions

### EBNF Symbols

| Symbol | Meaning | Example |
|--------|---------|---------|
| `::=` | Is defined as | `digit ::= '0' | '1' | '2'` |
| `|` | Alternative (OR) | `bool ::= '0' | '1'` |
| `( )` | Grouping | `sign ::= ('+' | '-')` |
| `[ ]` | Optional (0 or 1) | `number ::= [sign] digit+` |
| `{ }` | Repetition (0 or more) | `digits ::= { digit }` |
| `+` | One or more | `identifier ::= letter+` |
| `*` | Zero or more | `whitespace ::= ' '*` |
| `' '` | Terminal symbol | `'func'` |
| `" "` | String literal | `"Hello"` |

### Meta-Symbols

- `ε` (epsilon) = empty/null production
- `letter` = `[a-zA-Z_]`
- `digit` = `[0-9]`
- `any` = any printable character

---

## 2. Lexical Grammar

### 2.1 Basic Tokens

```ebnf
(* Whitespace and Comments *)
whitespace ::= ' ' | '\t' | '\r' | '\n'
comment    ::= '//' { any - '\n' } '\n'
           |   '/*' { any - '*/' } '*/'

(* Identifiers *)
letter     ::= 'a'..'z' | 'A'..'Z' | '_'
digit      ::= '0'..'9'
identifier ::= letter { letter | digit }

(* Keywords *)
keyword    ::= 'func' | 'var' | 'return' | 'for' | 'if' 
           |   'else' | 'while' | 'print' | 'break' | 'continue'

(* Type Keywords *)
type_keyword ::= 'int' | 'char' | 'byte' | 'bit' 
             |   'float' | 'double' | 'string' | 'void'
```

### 2.2 Literals

```ebnf
(* Integer Literals *)
sign         ::= '+' | '-'
non_zero     ::= '1'..'9'
integer      ::= '0' | [sign] non_zero { digit }

(* Float Literals *)
exponent     ::= ('e' | 'E') [sign] digit+
float        ::= [sign] digit+ '.' digit+ [exponent]

(* Character Literals *)
escape_seq   ::= '\\' ('n' | 't' | 'r' | '0' | '\\' | "'" | '"')
char_content ::= (any - "'" - '\\') | escape_seq
char_literal ::= "'" char_content "'"

(* String Literals *)
string_char  ::= (any - '"' - '\\') | escape_seq
string       ::= '"' { string_char } '"'

(* Boolean Literals *)
boolean      ::= '0' | '1'
```

### 2.3 Operators and Punctuation

```ebnf
(* Arithmetic Operators *)
plus         ::= '+'
minus        ::= '-'
star         ::= '*'
slash        ::= '/'
percent      ::= '%'

(* Comparison Operators *)
equal_equal  ::= '=='
bang_equal   ::= '!='
less         ::= '<'
less_equal   ::= '<='
greater      ::= '>'
greater_equal ::= '>='

(* Logical Operators *)
and_and      ::= '&&'
pipe_pipe    ::= '||'
bang         ::= '!'

(* Assignment Operators *)
equal        ::= '='
plus_equal   ::= '+='
minus_equal  ::= '-='
star_equal   ::= '*='
slash_equal  ::= '/='

(* Increment/Decrement *)
plus_plus    ::= '++'
minus_minus  ::= '--'

(* Punctuation *)
semicolon    ::= ';'
comma        ::= ','
colon        ::= ':'
arrow        ::= '->'
lparen       ::= '('
rparen       ::= ')'
lbrace       ::= '{'
rbrace       ::= '}'
```

---

## 3. Syntactic Grammar

### 3.1 Program Structure

```ebnf
(* Top-Level Structure *)
program          ::= { function }

function         ::= 'func' identifier '(' parameter_list ')' 
                     '->' type function_body

parameter_list   ::= ε
                 |   parameter { ',' parameter }

parameter        ::= identifier ':' type

function_body    ::= '{' { statement } '}'

type             ::= 'int' | 'char' | 'byte' | 'bit' 
                 |   'float' | 'double' | 'string' | 'void'
```

### 3.2 Statements

```ebnf
statement        ::= var_declaration
                 |   assignment
                 |   for_loop
                 |   if_statement
                 |   return_statement
                 |   print_statement
                 |   expression_statement
                 |   break_statement
                 |   continue_statement

expression_statement ::= function_call ';'

(* Variable Declaration *)
var_declaration  ::= 'var' identifier [':' type] ['=' expression] ';'

(* Assignment *)
assignment       ::= identifier assignment_op expression ';'
                 |   identifier ('++' | '--') ';'

assignment_op    ::= '=' | '+=' | '-=' | '*=' | '/='

(* For Loop *)
for_loop         ::= 'for' '(' for_init ';' expression ';' for_update ')' 
                     '{' { statement } '}'

for_init         ::= 'var' identifier [':' type] '=' expression
                 |   identifier '=' expression

for_update       ::= identifier '++'
                 |   identifier '--'
                 |   identifier assignment_op expression

(* If Statement *)
if_statement     ::= 'if' '(' expression ')' 
                     '{' { statement } '}' 
                     ['else' '{' { statement } '}']

(* Return Statement *)
return_statement ::= 'return' [expression] ';'

(* Print Statement *)
print_statement  ::= 'print' '(' print_expression ')' ';'

print_expression ::= string_literal
                 |   expression
                 |   string_literal '+' expression
                 |   expression '+' print_expression
                 |   string_literal '+' print_expression

(* Break Statement *)
break_statement  ::= 'break' ';'

(* Continue Statement *)
continue_statement ::= 'continue' ';'
```

### 3.3 Expressions (with Precedence)

```ebnf
(* Expression Hierarchy (Lowest to Highest Precedence) *)

expression       ::= logical_or

logical_or       ::= logical_and { '||' logical_and }

logical_and      ::= comparison { '&&' comparison }

comparison       ::= term { comparison_op term }

comparison_op    ::= '==' | '!=' | '<' | '<=' | '>' | '>='

term             ::= factor { ('+' | '-') factor }

factor           ::= unary { ('*' | '/' | '%') unary }

unary            ::= '!' unary
                 |   '-' unary
                 |   primary

primary          ::= integer
                 |   float
                 |   char_literal
                 |   string_literal
                 |   boolean
                 |   identifier
                 |   function_call
                 |   '(' expression ')'

function_call    ::= identifier '(' argument_list ')'

argument_list    ::= ε
                 |   expression { ',' expression }
```

---

## 4. Operator Precedence

### 4.1 Precedence Table (Highest to Lowest)

```ebnf
(* Precedence Level 1 (Highest) - Primary *)
primary          ::= literals | identifiers | parentheses | function_calls

(* Precedence Level 2 - Unary *)
unary            ::= '!' | '-' (unary_minus)

(* Precedence Level 3 - Multiplicative *)
multiplicative   ::= '*' | '/' | '%'

(* Precedence Level 4 - Additive *)
additive         ::= '+' | '-'

(* Precedence Level 5 - Comparison *)
comparison       ::= '<' | '<=' | '>' | '>=' | '==' | '!='

(* Precedence Level 6 - Logical AND *)
logical_and      ::= '&&'

(* Precedence Level 7 (Lowest) - Logical OR *)
logical_or       ::= '||'
```

### 4.2 Associativity

```ebnf
(* Left-Associative Operators *)
left_assoc       ::= '*' | '/' | '%' | '+' | '-' 
                 |   '<' | '<=' | '>' | '>=' | '==' | '!='
                 |   '&&' | '||'

(* Right-Associative Operators *)
right_assoc      ::= '!' | '-' (unary)

(* Non-Associative Operators *)
non_assoc        ::= '=' | '+=' | '-=' | '*=' | '/='
```

---

## 5. Type System Grammar

### 5.1 Type Rules

```ebnf
(* Primitive Types *)
primitive_type   ::= 'int' | 'char' | 'byte' | 'bit' 
                 |   'float' | 'double' | 'string'

(* Function Types *)
function_type    ::= '(' type_list ')' '->' type

type_list        ::= ε
                 |   type { ',' type }

(* Type Inference *)
inferred_type    ::= typeof(expression)

typeof(integer)       = 'int'
typeof(float)         = 'float'
typeof(char_literal)  = 'char'
typeof(string_literal) = 'string'
typeof(boolean)       = 'bit'
```

### 5.2 Type Compatibility

```ebnf
(* Type Assignment Rules *)
assignment_compatible ::= (type1, type2) where type1 = type2

(* Function Call Rules *)
call_compatible ::= function_type matches argument_types

function_type matches argument_types :=
    parameter_count = argument_count ∧
    ∀i: parameter_type[i] = argument_type[i]
```

---

## 6. Examples

### 6.1 Example Derivations

#### Example 1: Simple Variable Declaration

```
Input: var x:int = 42;

Derivation:
statement
→ var_declaration
→ 'var' identifier ':' type '=' expression ';'
→ 'var' 'x' ':' 'int' '=' expression ';'
→ 'var' 'x' ':' 'int' '=' logical_or ';'
→ 'var' 'x' ':' 'int' '=' logical_and ';'
→ 'var' 'x' ':' 'int' '=' comparison ';'
→ 'var' 'x' ':' 'int' '=' term ';'
→ 'var' 'x' ':' 'int' '=' factor ';'
→ 'var' 'x' ':' 'int' '=' unary ';'
→ 'var' 'x' ':' 'int' '=' primary ';'
→ 'var' 'x' ':' 'int' '=' integer ';'
→ 'var' 'x' ':' 'int' '=' '42' ';'
```

#### Example 2: Expression with Operators

```
Input: x + y * 3

Derivation:
expression
→ logical_or
→ logical_and
→ comparison
→ term
→ term '+' factor
→ factor '+' factor
→ unary '+' factor
→ primary '+' factor
→ identifier '+' factor
→ 'x' '+' factor
→ 'x' '+' factor '*' unary
→ 'x' '+' unary '*' unary
→ 'x' '+' primary '*' unary
→ 'x' '+' identifier '*' unary
→ 'x' '+' 'y' '*' unary
→ 'x' '+' 'y' '*' primary
→ 'x' '+' 'y' '*' integer
→ 'x' '+' 'y' '*' '3'
```

#### Example 3: String Function

```
Input: func greet(name:string) -> void { print("Hello, " + name); }

Derivation:
function
→ 'func' identifier '(' parameter_list ')' '->' type function_body
→ 'func' 'greet' '(' parameter ')' '->' type function_body
→ 'func' 'greet' '(' identifier ':' type ')' '->' type function_body
→ 'func' 'greet' '(' 'name' ':' 'string' ')' '->' 'void' function_body
→ 'func' 'greet' '(' 'name' ':' 'string' ')' '->' 'void' '{' statement '}'
→ 'func' 'greet' '(' 'name' ':' 'string' ')' '->' 'void' '{' print_statement '}'
→ 'func' 'greet' '(' 'name' ':' 'string' ')' '->' 'void' 
  '{' 'print' '(' print_expression ')' ';' '}'
→ 'func' 'greet' '(' 'name' ':' 'string' ')' '->' 'void' 
  '{' 'print' '(' string_literal '+' expression ')' ';' '}'
→ 'func' 'greet' '(' 'name' ':' 'string' ')' '->' 'void' 
  '{' 'print' '(' '"Hello, "' '+' identifier ')' ';' '}'
→ 'func' 'greet' '(' 'name' ':' 'string' ')' '->' 'void' 
  '{' 'print' '(' '"Hello, "' '+' 'name' ')' ';' '}'
```

#### Example 4: Logical Expression

```
Input: x > 0 && y < 10

Derivation:
expression
→ logical_or
→ logical_and
→ logical_and '&&' comparison
→ comparison '&&' comparison
→ term comparison_op term '&&' comparison
→ factor '>' factor '&&' comparison
→ primary '>' primary '&&' comparison
→ identifier '>' integer '&&' comparison
→ 'x' '>' '0' '&&' comparison
→ 'x' '>' '0' '&&' term comparison_op term
→ 'x' '>' '0' '&&' factor '<' factor
→ 'x' '>' '0' '&&' primary '<' primary
→ 'x' '>' '0' '&&' identifier '<' integer
→ 'x' '>' '0' '&&' 'y' '<' '10'
```

---

## 7. Ambiguity Resolution

### 7.1 Dangling Else

```ebnf
(* Problem: *)
if (x) if (y) A else B

(* Resolution: else binds to closest if *)
if_statement ::= 'if' '(' expression ')' matched_statement
             |   'if' '(' expression ')' unmatched_statement

matched_statement ::= 'if' '(' expression ')' matched_statement 
                      'else' matched_statement
                  |   other_statement

unmatched_statement ::= 'if' '(' expression ')' statement
                    |   'if' '(' expression ')' matched_statement 
                        'else' unmatched_statement
```

### 7.2 Operator Precedence

```ebnf
(* Ambiguous: a + b * c *)
(* Could be: (a + b) * c  OR  a + (b * c) *)

(* Resolution: Use precedence levels *)
(* Correct: a + (b * c) *)

term   ::= factor { '+' factor }      (* Lower precedence *)
factor ::= unary { '*' unary }        (* Higher precedence *)
```

---

## 8. Grammar Properties

### 8.1 Grammar Classification

- **Type:** Context-Free Grammar (CFG)
- **Class:** LL(2) - Lookahead of 2 tokens needed for some productions
- **Parsing Method:** Recursive Descent with Operator Precedence Climbing
- **Ambiguity:** Unambiguous (precedence and associativity rules applied)

### 8.2 First and Follow Sets (Examples)

```ebnf
(* FIRST Sets *)
FIRST(statement)     = { 'var', identifier, 'for', 'if', 'return', 'print' }
FIRST(expression)    = { integer, float, char, string, identifier, '(', '!', '-' }
FIRST(type)          = { 'int', 'char', 'byte', 'bit', 'float', 'double', 'string', 'void' }

(* FOLLOW Sets *)
FOLLOW(expression)   = { ';', ')', ',', ']', '}', '+', '-', '*', '/', '%', 
                         '==', '!=', '<', '<=', '>', '>=', '&&', '||' }
FOLLOW(statement)    = { '}', 'else' }
FOLLOW(type)         = { identifier, ')', ',' }
```

### 8.3 Nullable Productions

```ebnf
(* Nullable non-terminals (can derive ε) *)
nullable = { parameter_list, argument_list, expression (in return) }

parameter_list → ε
argument_list  → ε
expression     → ε  (only in 'return ;')
```

---

## 9. Railroad Diagrams (Visual Grammar)

### 9.1 Expression

```
expression:
    ┌─────────────────────────┐
    │                         │
    └→ logical_or ────────────┘

logical_or:
    ┌──────────────────────────┐
    │           ┌───'||'───┐   │
    └→ logical_and ─┴──────┴───┘

logical_and:
    ┌────────────────────────┐
    │        ┌───'&&'───┐    │
    └→ comparison ─┴──────┴──┘

comparison:
    ┌───────────────────────────────────┐
    │     ┌─ '==' ─┐  ┌─ '!=' ─┐       │
    │     ├─ '<'  ─┤  ├─ '<=' ─┤       │
    └→ term ┴─ '>'  ─┴──┴─ '>=' ─┴─────┘

term:
    ┌────────────────────────┐
    │      ┌─ '+' ─┐         │
    └→ factor ┴─ '-' ─┴──────┘

factor:
    ┌─────────────────────────────┐
    │      ┌─ '*' ─┐  ┌─ '%' ─┐  │
    └→ unary ┴─ '/' ─┴──┴───────┴─┘

unary:
       ┌─ '!' ──┐
       ├─ '-' ──┤
       └─────────┴→ primary
```

### 9.2 Statement

```
statement:
    ┌─ var_declaration ───┐
    ├─ assignment ────────┤
    ├─ for_loop ──────────┤
    ├─ if_statement ──────┤
    ├─ return_statement ──┤
    ├─ print_statement ───┤
    └─ expression_stmt ───┘
```

---

## 10. Formal Language Definition

### 10.1 Alphabet

```
Σ = { 'a'..'z', 'A'..'Z', '0'..'9', '_', '+', '-', '*', '/', '%', 
      '=', '<', '>', '!', '&', '|', '(', ')', '{', '}', '[', ']',
      ';', ',', ':', '.', '"', "'", '\\', ' ', '\t', '\n', '\r' }
```

### 10.2 Language

```
L(Grammar) = { w ∈ Σ* | program ⇒* w }

Where:
- program is the start symbol
- ⇒* denotes zero or more derivation steps
- w is a valid program string
```

### 10.3 Valid Program Conditions

```
A program P is valid if and only if:

1. P ∈ L(Grammar)                    (syntactically correct)
2. ∃ main_func ∈ P                   (contains main function)
3. ∀ var ∈ used_vars(P):             (all variables declared)
     ∃ decl ∈ declarations(P): var ∈ decl
4. ∀ func_call ∈ P:                  (all functions exist)
     ∃ func_def ∈ P: name(func_call) = name(func_def)
5. ∀ expr ∈ P: type_check(expr)      (type-safe)
```

---

# 🎉 FORMAL GRAMMAR SPECIFICATION END 🎉

**Author:** Philipp01105  
**Date:** 2025-10-16 07:47:41 UTC  
**Version:** 4.0.0  
**Grammar Class:** LL(2) Context-Free Grammar  
**Parsing Method:** Recursive Descent with Operator Precedence Climbing

**Summary:**
- ✅ Complete lexical grammar (tokens, literals, operators)
- ✅ Complete syntactic grammar (program structure, statements, expressions)
- ✅ Operator precedence and associativity rules
- ✅ Type system grammar
- ✅ Derivation examples
- ✅ Ambiguity resolution rules
- ✅ Grammar properties (FIRST/FOLLOW sets)
- ✅ Railroad diagrams (visual representation)
- ✅ Formal language definition

*This grammar specification fully defines the language accepted by compiler version 4.0.0*