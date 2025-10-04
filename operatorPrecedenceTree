EXPRESSION (Precedence: 1 - niedrigste)
│
└─── LOGICAL_OR (||)
     │
     ├─── Left:  LOGICAL_AND
     ├─── Op:    "||"
     └─── Right: LOGICAL_AND
          │
          └─── LOGICAL_AND (&&)  (Precedence: 2)
               │
               ├─── Left:  COMPARISON
               ├─── Op:    "&&"
               └─── Right: COMPARISON
                    │
                    └─── COMPARISON  (Precedence: 3)
                         │
                         ├─── Left:  TERM
                         ├─── Op:    "<" | "<=" | ">" | ">=" | "==" | "!="
                         └─── Right: TERM
                              │
                              └─── TERM  (Precedence: 4)
                                   │
                                   ├─── Left:  FACTOR
                                   ├─── Op:    "+" | "-"
                                   └─── Right: FACTOR
                                        │
                                        └─── FACTOR  (Precedence: 5)
                                             │
                                             ├─── Left:  UNARY
                                             ├─── Op:    "*" | "/"
                                             └─── Right: UNARY
                                                  │
                                                  └─── UNARY  (Precedence: 6)
                                                       │
                                                       ├─── "!" UNARY
                                                       ├─── "-" UNARY (negation)
                                                       └─── PRIMARY
                                                            │
                                                            └─── PRIMARY  (Precedence: 7 - höchste)
                                                                 │
                                                                 ├─── "(" EXPRESSION ")"
                                                                 ├─── FUNCTION_CALL
                                                                 ├─── IDENTIFIER
                                                                 ├─── INTEGER
                                                                 ├─── FLOAT
                                                                 └─── CHAR