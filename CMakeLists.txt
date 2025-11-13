cmake_minimum_required(VERSION 3.10)
project(compiler C)

set(CMAKE_C_STANDARD 23)

add_executable(compiler
        src/lexer.c
        src/lexer.h
        src/compiler_types.h
        src/main.c
        src/parser.c
        src/parser.h
        src/errorHandler.c
        src/errorHandler.h
)
