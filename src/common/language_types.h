#ifndef DMM_LANGUAGE_TYPES_H
#define DMM_LANGUAGE_TYPES_H
#include <stdint.h>

#define MAX_TOKEN 512

typedef enum {
    TOKEN_KEYWORD_FUNC, TOKEN_KEYWORD_VAR, TOKEN_KEYWORD_RETURN,
    TOKEN_KEYWORD_FOR, TOKEN_KEYWORD_IF, TOKEN_KEYWORD_ELSE,
    TOKEN_KEYWORD_WHILE,
    TOKEN_KEYWORD_BREAK, TOKEN_KEYWORD_CONTINUE, TOKEN_KEYWORD_STRUCT,
    TOKEN_KEYWORD_ENUM, TOKEN_KEYWORD_IMPORT, TOKEN_KEYWORD_STATIC,
    TOKEN_KEYWORD_RESERVE, TOKEN_KEYWORD_FREE,
    TOKEN_KEYWORD_CONST, TOKEN_KEYWORD_PACKAGE, TOKEN_KEYWORD_PUB,
    TOKEN_KEYWORD_SIZEOF, TOKEN_KEYWORD_TYPEOF, TOKEN_KEYWORD_ALIGNOF, TOKEN_KEYWORD_SLICE, TOKEN_KEYWORD_CASE,
    TOKEN_TYPE_INT, TOKEN_TYPE_CHAR, TOKEN_TYPE_BYTE, TOKEN_TYPE_BIT,
    TOKEN_TYPE_FLOAT, TOKEN_TYPE_DOUBLE, TOKEN_TYPE_STRING,
    TOKEN_TYPE_I8, TOKEN_TYPE_U8, TOKEN_TYPE_I16, TOKEN_TYPE_U16,
    TOKEN_TYPE_I32, TOKEN_TYPE_U32, TOKEN_TYPE_I64, TOKEN_TYPE_U64,
    TOKEN_TYPE_ISIZE, TOKEN_TYPE_USIZE, TOKEN_TYPE_VOID,
    TOKEN_IDENTIFIER, TOKEN_NUMBER, TOKEN_FLOAT_LITERAL, TOKEN_CHAR_LITERAL,
    TOKEN_STRING_LITERAL, TOKEN_PLUS, TOKEN_MINUS, TOKEN_STAR, TOKEN_SLASH,
    TOKEN_PERCENT, TOKEN_EQUAL, TOKEN_EQUAL_EQUAL, TOKEN_BANG_EQUAL,
    TOKEN_LESS, TOKEN_LESS_EQUAL, TOKEN_GREATER, TOKEN_GREATER_EQUAL,
    TOKEN_AMP_AMP, TOKEN_PIPE_PIPE, TOKEN_BANG, TOKEN_AMPERSAND,
    TOKEN_PLUS_EQUAL, TOKEN_MINUS_EQUAL, TOKEN_STAR_EQUAL, TOKEN_SLASH_EQUAL,
    TOKEN_PLUS_PLUS, TOKEN_MINUS_MINUS, TOKEN_LPAREN, TOKEN_RPAREN,
    TOKEN_LBRACE, TOKEN_RBRACE, TOKEN_LBRACKET, TOKEN_RBRACKET,
    TOKEN_SEMICOLON, TOKEN_COMMA, TOKEN_COLON, TOKEN_ARROW, TOKEN_DOT,
    TOKEN_KEYWORD_TRAIT, TOKEN_KEYWORD_IMPL, TOKEN_KEYWORD_MATCH, TOKEN_FAT_ARROW,
    TOKEN_HASH, TOKEN_AT, TOKEN_COMMENT, TOKEN_NEWLINE, TOKEN_EOF, TOKEN_ERROR
} TokenType;

typedef enum {
    TYPE_INT, TYPE_CHAR, TYPE_BYTE, TYPE_BIT, TYPE_FLOAT, TYPE_DOUBLE,
    TYPE_STRING, TYPE_I8, TYPE_U8, TYPE_I16, TYPE_U16, TYPE_I32, TYPE_U32,
    TYPE_I64, TYPE_U64, TYPE_ISIZE, TYPE_USIZE, TYPE_VOID, TYPE_UNKNOWN
} DataType;

#define DMM_TYPE_NAMES "int", "char", "byte", "bit", "float", "double", "string", \
    "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "isize", "usize", "void", "unknown"

static inline int data_type_integral(DataType type) {
    return type == TYPE_INT || type == TYPE_CHAR || type == TYPE_BYTE || type == TYPE_BIT ||
           (type >= TYPE_I8 && type <= TYPE_USIZE);
}
static inline int data_type_fixed_integer(DataType type) {
    return type >= TYPE_I8 && type <= TYPE_USIZE;
}
static inline int data_type_unsigned(DataType type) {
    return type == TYPE_BYTE || type == TYPE_BIT || type == TYPE_U8 || type == TYPE_U16 ||
           type == TYPE_U32 || type == TYPE_U64 || type == TYPE_USIZE;
}
static inline unsigned data_type_bytes(DataType type) {
    if (type == TYPE_CHAR || type == TYPE_BYTE || type == TYPE_BIT || type == TYPE_I8 || type == TYPE_U8) return 1;
    if (type == TYPE_I16 || type == TYPE_U16) return 2;
    if (type == TYPE_INT || type == TYPE_I32 || type == TYPE_U32 || type == TYPE_FLOAT) return 4;
    return 8;
}
static inline uint64_t data_type_normalize_integer(uint64_t bits, DataType type) {
    unsigned width = data_type_bytes(type) * 8;
    if (width == 64) return bits;
    uint64_t mask = (UINT64_C(1) << width) - 1;
    bits &= mask;
    if (!data_type_unsigned(type)) {
        uint64_t sign = UINT64_C(1) << (width - 1);
        bits = (bits ^ sign) - sign;
    }
    return bits;
}
static inline DataType token_data_type(TokenType token) {
    return token >= TOKEN_TYPE_INT && token <= TOKEN_TYPE_VOID ?
           (DataType)(token - TOKEN_TYPE_INT) : TYPE_UNKNOWN;
}
static inline DataType data_type_promoted_integer(DataType left, DataType right) {
    if (!data_type_fixed_integer(left) && !data_type_fixed_integer(right)) return TYPE_INT;
    if (left == right) return left;
    unsigned a = data_type_bytes(left), b = data_type_bytes(right), width = a > b ? a : b;
    int unsign = (data_type_unsigned(left) && a >= b) || (data_type_unsigned(right) && b >= a);
    if (width == 8 && (left == TYPE_USIZE || right == TYPE_USIZE) && unsign) return TYPE_USIZE;
    if (width == 8 && (left == TYPE_ISIZE || right == TYPE_ISIZE) && !unsign) return TYPE_ISIZE;
    return width == 1 ? (unsign ? TYPE_U8 : TYPE_I8) : width == 2 ? (unsign ? TYPE_U16 : TYPE_I16) :
           width == 4 ? (unsign ? TYPE_U32 : TYPE_I32) : (unsign ? TYPE_U64 : TYPE_I64);
}

typedef enum { TARGET_ELF, TARGET_COFF } TargetFormat;
typedef enum { SYNTAX_ATT, SYNTAX_INTEL } SyntaxMode;

#endif
