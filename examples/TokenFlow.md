INPUT: "var x:int = 10;"

TOKEN_STREAM:
TOKEN_KEYWORD_VAR
TOKEN_IDENTIFIER("x")
TOKEN_COLON
TOKEN_TYPE_INT
TOKEN_EQUAL
TOKEN_NUMBER("10")
TOKEN_SEMICOLON

PARSE_FLOW:
parse_statement()
├─── Erkennt: TOKEN_KEYWORD_VAR
└─── Ruft auf: parse_variable_declaration()
│
├─── consume() → TOKEN_KEYWORD_VAR
├─── consume() → TOKEN_IDENTIFIER("x")
├─── check(TOKEN_COLON) → true
├─── consume() → TOKEN_COLON
├─── consume() → TOKEN_TYPE_INT
├─── check(TOKEN_EQUAL) → true
├─── consume() → TOKEN_EQUAL
│
├─── Ruft auf: parse_expression()
│    │
│    └─── parse_logical_or()
│         └─── parse_logical_and()
│              └─── parse_comparison()
│                   └─── parse_term()
│                        └─── parse_factor()
│                             └─── parse_unary()
│                                  └─── parse_primary()
│                                       │
│                                       └─── check(TOKEN_NUMBER)
│                                            consume() → TOKEN_NUMBER("10")
│                                            pushq %rax
│
└─── consume() → TOKEN_SEMICOLON

RESULT: Variable "x" deklariert und initialisiert