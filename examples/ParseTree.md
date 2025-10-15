VARIABLE_DECLARATION
│
├─── "var"
├─── IDENTIFIER: "x"
├─── ":"
├─── TYPE: "int"
├─── "="
├─── EXPRESSION
│    │
│    └─── LOGICAL_OR
│         │
│         └─── LOGICAL_AND
│              │
│              └─── COMPARISON
│                   │
│                   └─── TERM
│                        │
│                        └─── FACTOR
│                             │
│                             ├─── UNARY
│                             │    │
│                             │    └─── PRIMARY
│                             │         │
│                             │         └─── "(" EXPRESSION ")"
│                             │              │
│                             │              └─── LOGICAL_OR
│                             │                   │
│                             │                   └─── ... (rekursiv)
│                             │                        │
│                             │                        └─── TERM
│                             │                             │
│                             │                             ├─── FACTOR
│                             │                             │    │
│                             │                             │    └─── UNARY
│                             │                             │         │
│                             │                             │         └─── PRIMARY
│                             │                             │              │
│                             │                             │              └─── IDENTIFIER: "a"
│                             │                             │
│                             │                             ├─── "+"
│                             │                             │
│                             │                             └─── FACTOR
│                             │                                  │
│                             │                                  └─── UNARY
│                             │                                       │
│                             │                                       └─── PRIMARY
│                             │                                            │
│                             │                                            └─── IDENTIFIER: "b"
│                             │
│                             ├─── "*"
│                             │
│                             └─── UNARY
│                                  │
│                                  └─── PRIMARY
│                                       │
│                                       └─── INTEGER: "2"
│
└─── ";"




FOR_LOOP
│
├─── "for"
├─── "("
│
├─── FOR_INIT
│    │
│    ├─── "var"
│    ├─── IDENTIFIER: "i"
│    ├─── "="
│    └─── EXPRESSION
│         │
│         └─── PRIMARY: INTEGER "0"
│
├─── ";"
│
├─── CONDITION (EXPRESSION)
│    │
│    └─── COMPARISON
│         │
│         ├─── TERM: IDENTIFIER "i"
│         ├─── "<"
│         └─── TERM: INTEGER "10"
│
├─── ";"
│
├─── FOR_UPDATE
│    │
│    ├─── IDENTIFIER: "i"
│    └─── "++"
│
├─── ")"
├─── "{"
│
├─── STATEMENT*
│    │
│    └─── (body statements)
│
└─── "}"



FUNCTION_CALL
│
├─── IDENTIFIER: "add"
├─── "("
│
├─── ARGUMENT_LIST
│    │
│    ├─── EXPRESSION (arg 1)
│    │    │
│    │    └─── PRIMARY: INTEGER "10"
│    │
│    ├─── ","
│    │
│    └─── EXPRESSION (arg 2)
│         │
│         └─── PRIMARY
│              │
│              └─── FUNCTION_CALL
│                   │
│                   ├─── IDENTIFIER: "multiply"
│                   ├─── "("
│                   │
│                   ├─── ARGUMENT_LIST
│                   │    │
│                   │    ├─── EXPRESSION: INTEGER "2"
│                   │    ├─── ","
│                   │    └─── EXPRESSION: INTEGER "3"
│                   │
│                   └─── ")"
│
└─── ")"



PROGRAM
│
├─── FUNCTION: "add"
│    │
│    ├─── PARAMETERS
│    │    │
│    │    ├─── PARAM: "a:int"
│    │    └─── PARAM: "b:int"
│    │
│    ├─── RETURN_TYPE: "int"
│    │
│    └─── BODY
│         │
│         └─── RETURN_STATEMENT
│              │
│              └─── EXPRESSION
│                   │
│                   └─── TERM
│                        │
│                        ├─── IDENTIFIER: "a"
│                        ├─── "+"
│                        └─── IDENTIFIER: "b"
│
│
└─── FUNCTION: "main"
│
├─── PARAMETERS: (none)
├─── RETURN_TYPE: "void"
│
└─── BODY
│
├─── VARIABLE_DECLARATION
│    │
│    ├─── NAME: "x"
│    ├─── TYPE: "int"
│    └─── INITIALIZER: INTEGER "10"
│
├─── VARIABLE_DECLARATION
│    │
│    ├─── NAME: "y"
│    ├─── TYPE: "int"
│    └─── INITIALIZER: INTEGER "20"
│
├─── VARIABLE_DECLARATION
│    │
│    ├─── NAME: "sum"
│    ├─── TYPE: "int"
│    └─── INITIALIZER
│         │
│         └─── FUNCTION_CALL
│              │
│              ├─── NAME: "add"
│              └─── ARGUMENTS
│                   │
│                   ├─── IDENTIFIER: "x"
│                   └─── IDENTIFIER: "y"
│
└─── PRINT_STATEMENT
│
└─── PRINT_EXPRESSION
│
├─── STRING: "Sum = "
├─── "+"
└─── IDENTIFIER: "sum"