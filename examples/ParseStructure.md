┌─────────────────────────────────────────────────┐
│           RECURSIVE DESCENT PARSER              │
├─────────────────────────────────────────────────┤
│                                                 │
│  ┌───────────────────────────────────────┐    │
│  │   parse_program()                     │    │
│  │   ├─── while (!is_at_end())           │    │
│  │   └─── parse_function()               │    │
│  └───────────────────────────────────────┘    │
│           │                                     │
│           ▼                                     │
│  ┌───────────────────────────────────────┐    │
│  │   parse_function()                    │    │
│  │   ├─── func <name>(<params>) -> <ret> │    │
│  │   └─── parse_function_body()          │    │
│  └───────────────────────────────────────┘    │
│           │                                     │
│           ▼                                     │
│  ┌───────────────────────────────────────┐    │
│  │   parse_statement()                   │    │
│  │   ├─── if (var)  → parse_var_decl()   │    │
│  │   ├─── if (id)   → parse_assignment() │    │
│  │   ├─── if (for)  → parse_for_loop()   │    │
│  │   ├─── if (if)   → parse_if_stmt()    │    │
│  │   ├─── if (ret)  → parse_return()     │    │
│  │   └─── if (print)→ parse_print()      │    │
│  └───────────────────────────────────────┘    │
│           │                                     │
│           ▼                                     │
│  ┌───────────────────────────────────────┐    │
│  │   parse_expression()                  │    │
│  │   (Operator Precedence Climbing)      │    │
│  │                                        │    │
│  │   parse_logical_or()         (1)      │    │
│  │     └─ parse_logical_and()   (2)      │    │
│  │          └─ parse_comparison() (3)    │    │
│  │               └─ parse_term()    (4)  │    │
│  │                    └─ parse_factor()(5)│    │
│  │                         └─ parse_unary()(6)│
│  │                              └─ parse_primary()(7)│
│  └───────────────────────────────────────┘    │
│                                                 │
└─────────────────────────────────────────────────┘