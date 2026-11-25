#pragma once

#include <stddef.h>

typedef enum {
    TOKEN_FUN,
    TOKEN_RETURN,
    TOKEN_IF,
    TOKEN_ELSE,
    TOKEN_FOR,
    TOKEN_IDENTIFIER,
    TOKEN_INTEGER,
    TOKEN_FLOAT,
    TOKEN_STRING,
    TOKEN_COMMENT,
    TOKEN_LEFT_PAREN,
    TOKEN_RIGHT_PAREN,
    TOKEN_COLON,
    TOKEN_COMMA,
    TOKEN_SEMICOLON,
    TOKEN_DOT,
    TOKEN_LEFT_BRACE,
    TOKEN_RIGHT_BRACE,
    TOKEN_LEFT_BRACKET,
    TOKEN_RIGHT_BRACKET,
    TOKEN_PLUS,
    TOKEN_MINUS,
    TOKEN_STAR,
    TOKEN_SLASH,
    TOKEN_PERCENT,
    TOKEN_EQUAL,
    TOKEN_EQUAL_EQUAL,
    TOKEN_BANG,
    TOKEN_BANG_EQUAL,
    TOKEN_LESS,
    TOKEN_LESS_EQUAL,
    TOKEN_GREATER,
    TOKEN_GREATER_EQUAL,
    TOKEN_AMPERSAND,
    TOKEN_AMPERSAND_AMPERSAND,
    TOKEN_PIPE,
    TOKEN_PIPE_PIPE,
    TOKEN_CARET,
    TOKEN_TILDE,
    TOKEN_SHIFT_LEFT,
    TOKEN_SHIFT_RIGHT,
    TOKEN_PLUS_EQUAL,
    TOKEN_MINUS_EQUAL,
    TOKEN_STAR_EQUAL,
    TOKEN_SLASH_EQUAL,
    TOKEN_PERCENT_EQUAL,
    TOKEN_AMPERSAND_EQUAL,
    TOKEN_PIPE_EQUAL,
    TOKEN_CARET_EQUAL,
    TOKEN_SHIFT_LEFT_EQUAL,
    TOKEN_SHIFT_RIGHT_EQUAL,
    TOKEN_NEWLINE,
    TOKEN_EOF,
    TOKEN_INVALID,
    TOKEN_INVALID_NUMBER,
    TOKEN_UNTERMINATED_STRING,
    TOKEN_UNTERMINATED_COMMENT,
} TokenKind;

typedef struct {
    TokenKind kind;
    size_t offset;
    size_t length;
    size_t line;
    size_t column;
} Token;

typedef struct {
    const unsigned char *source;
    size_t length;
    size_t offset;
    size_t line;
    size_t column;
} Lexer;

Lexer lexer_init(const unsigned char *source, size_t length);
Token lexer_next(Lexer *lexer);
