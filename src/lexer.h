#ifndef LUNSO_LEXER_H
#define LUNSO_LEXER_H

#include <stddef.h>

typedef enum {
    TOKEN_FUN,
    TOKEN_IDENTIFIER,
    TOKEN_LEFT_PAREN,
    TOKEN_RIGHT_PAREN,
    TOKEN_COLON,
    TOKEN_LEFT_BRACE,
    TOKEN_RIGHT_BRACE,
    TOKEN_NEWLINE,
    TOKEN_EOF,
    TOKEN_INVALID,
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

#endif
