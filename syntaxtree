PROGRAM
│
├─── FUNCTION*
│    │
│    ├─── "func"
│    ├─── IDENTIFIER (function name)
│    ├─── "("
│    ├─── PARAMETER_LIST?
│    │    │
│    │    └─── PARAMETER ("," PARAMETER)*
│    │         │
│    │         ├─── IDENTIFIER (param name)
│    │         ├─── ":"
│    │         └─── TYPE
│    │
│    ├─── ")"
│    ├─── "->"
│    ├─── TYPE (return type)
│    ├─── "{"
│    ├─── STATEMENT*
│    └─── "}"
│
└─── (repeat for each function)


TYPE
│
├─── "int"
├─── "char"
├─── "byte"
├─── "bit"
├─── "float"
├─── "double"
└─── "void"


STATEMENT
│
├─── VARIABLE_DECLARATION
│    │
│    ├─── "var"
│    ├─── IDENTIFIER
│    ├─── (":" TYPE)?
│    ├─── ("=" EXPRESSION)?
│    └─── ";"
│
├─── ASSIGNMENT
│    │
│    ├─── IDENTIFIER
│    ├─── ASSIGN_OPERATOR
│    │    │
│    │    ├─── "="
│    │    ├─── "+="
│    │    ├─── "-="
│    │    ├─── "*="
│    │    └─── "/="
│    │
│    ├─── EXPRESSION
│    └─── ";"
│
├─── INCREMENT/DECREMENT
│    │
│    ├─── IDENTIFIER
│    ├─── ("++" | "--")
│    └─── ";"
│
├─── FOR_LOOP
│    │
│    ├─── "for"
│    ├─── "("
│    ├─── FOR_INIT
│    │    │
│    │    ├─── "var" IDENTIFIER (":" TYPE)? "=" EXPRESSION
│    │    └─── IDENTIFIER "=" EXPRESSION
│    │
│    ├─── ";"
│    ├─── EXPRESSION (condition)
│    ├─── ";"
│    ├─── FOR_UPDATE
│    │    │
│    │    ├─── IDENTIFIER "++"
│    │    ├─── IDENTIFIER "--"
│    │    └─── IDENTIFIER ASSIGN_OP EXPRESSION
│    │
│    ├─── ")"
│    ├─── "{"
│    ├─── STATEMENT*
│    └─── "}"
│
├─── IF_STATEMENT
│    │
│    ├─── "if"
│    ├─── "("
│    ├─── EXPRESSION
│    ├─── ")"
│    ├─── "{"
│    ├─── STATEMENT*
│    ├─── "}"
│    ├─── ("else" "{" STATEMENT* "}")?
│    └───
│
├─── RETURN_STATEMENT
│    │
│    ├─── "return"
│    ├─── EXPRESSION?
│    └─── ";"
│
├─── PRINT_STATEMENT
│    │
│    ├─── "print"
│    ├─── "("
│    ├─── PRINT_EXPRESSION
│    │    │
│    │    ├─── STRING_LITERAL
│    │    ├─── EXPRESSION
│    │    └─── STRING "+" EXPRESSION ("+" ...)*
│    │
│    ├─── ")"
│    └─── ";"
│
└─── FUNCTION_CALL_STATEMENT
     │
     ├─── IDENTIFIER
     ├─── "("
     ├─── ARGUMENT_LIST?
     │    │
     │    └─── EXPRESSION ("," EXPRESSION)*
     │
     ├─── ")"
     └─── ";"


EXPRESSION (Operator Precedence Climbing)
│
└─── LOGICAL_OR
     │
     ├─── LOGICAL_AND
     │    │
     │    ├─── COMPARISON
     │    │    │
     │    │    ├─── TERM
     │    │    │    │
     │    │    │    ├─── FACTOR
     │    │    │    │    │
     │    │    │    │    ├─── UNARY
     │    │    │    │    │    │
     │    │    │    │    │    ├─── "!" UNARY
     │    │    │    │    │    ├─── "-" UNARY
     │    │    │    │    │    └─── PRIMARY
     │    │    │    │    │         │
     │    │    │    │    │         ├─── INTEGER_LITERAL
     │    │    │    │    │         ├─── FLOAT_LITERAL
     │    │    │    │    │         ├─── CHAR_LITERAL
     │    │    │    │    │         ├─── IDENTIFIER
     │    │    │    │    │         ├─── FUNCTION_CALL
     │    │    │    │    │         │    │
     │    │    │    │    │         │    ├─── IDENTIFIER
     │    │    │    │    │         │    ├─── "("
     │    │    │    │    │         │    ├─── ARGUMENT_LIST?
     │    │    │    │    │         │    └─── ")"
     │    │    │    │    │         │
     │    │    │    │    │         └─── "(" EXPRESSION ")"
     │    │    │    │    │
     │    │    │    │    └─── ("*" | "/") UNARY
     │    │    │    │
     │    │    │    └─── ("+" | "-") FACTOR
     │    │    │
     │    │    └─── ("<" | "<=" | ">" | ">=" | "==" | "!=") TERM
     │    │
     │    └─── "&&" COMPARISON
     │
     └─── "||" LOGICAL_AND


LITERAL
│
├─── INTEGER_LITERAL
│    │
│    └─── "-"? [0-9]+
│
├─── FLOAT_LITERAL
│    │
│    └─── "-"? [0-9]+ "." [0-9]+ ("e" [+-]? [0-9]+)?
│
├─── CHAR_LITERAL
│    │
│    ├─── "'" <char> "'"
│    └─── "'" ESCAPE_SEQUENCE "'"
│         │
│         └─── "\\" ("n" | "t" | "r" | "0" | "\\" | "'")
│
└─── STRING_LITERAL
     │
     └─── '"' [^"]* '"'


IDENTIFIER
│
└─── [a-zA-Z_][a-zA-Z0-9_]*