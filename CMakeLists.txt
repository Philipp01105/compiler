cmake_minimum_required(VERSION 3.10)
project(compiler C)

set(CMAKE_C_STANDARD 23)

add_executable(compiler
        src/lexer.c
        src/lexer.h
        src/compiler_types.h
        src/main.c
        src/parser_types.c
        src/parser_codegen.c
        src/parser_core.c
        src/parser_declarations.c
        src/parser_statements.c
        src/parser_expressions.c
        src/parser.h
        src/parser_internal.h
        src/errorHandler.c
        src/errorHandler.h
        src/asm_optimizer.c
        src/asm_optimizer.h
        src/instruction_builder.c
        src/instruction_builder.h
)
