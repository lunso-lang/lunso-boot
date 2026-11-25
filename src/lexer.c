#include "lexer.h"

#include <stdbool.h>
#include <string.h>

Lexer lexer_init(const unsigned char *source, size_t length)
{
    return (Lexer){.source = source, .length = length, .line = 1, .column = 1};
}

static unsigned char advance(Lexer *lexer)
{
    unsigned char byte = lexer->source[lexer->offset];
    lexer->offset++;
    lexer->column++;
    return byte;
}

static bool is_newline(unsigned char byte)
{
    return byte == '\n' || byte == '\r';
}

static bool is_identifier_start(unsigned char byte)
{
    return (byte >= 'a' && byte <= 'z') ||
           (byte >= 'A' && byte <= 'Z') || byte == '_';
}

static bool is_identifier_continue(unsigned char byte)
{
    return is_identifier_start(byte) || (byte >= '0' && byte <= '9');
}

static void skip_trivia(Lexer *lexer)
{
    while (lexer->offset < lexer->length) {
        unsigned char byte = lexer->source[lexer->offset];
        if (byte == ' ' || byte == '\t') {
            advance(lexer);
        } else if (byte == '/' && lexer->length - lexer->offset >= 2 &&
                   lexer->source[lexer->offset + 1] == '/') {
            do {
                advance(lexer);
            } while (lexer->offset < lexer->length &&
                     !is_newline(lexer->source[lexer->offset]));
        } else {
            return;
        }
    }
}

static TokenKind scan_token(Lexer *lexer)
{
    if (lexer->offset == lexer->length) {
        return TOKEN_EOF;
    }

    size_t start = lexer->offset;
    unsigned char byte = advance(lexer);
    switch (byte) {
    case '(':
        return TOKEN_LEFT_PAREN;
    case ')':
        return TOKEN_RIGHT_PAREN;
    case ':':
        return TOKEN_COLON;
    case '{':
        return TOKEN_LEFT_BRACE;
    case '}':
        return TOKEN_RIGHT_BRACE;
    case '\r':
    case '\n':
        if (byte == '\r' && lexer->offset < lexer->length &&
            lexer->source[lexer->offset] == '\n') {
            advance(lexer);
        }
        lexer->line++;
        lexer->column = 1;
        return TOKEN_NEWLINE;
    default:
        break;
    }

    if (!is_identifier_start(byte)) {
        return TOKEN_INVALID;
    }
    while (lexer->offset < lexer->length &&
           is_identifier_continue(lexer->source[lexer->offset])) {
        advance(lexer);
    }
    if (lexer->offset - start == 3 && memcmp(lexer->source + start, "fun", 3) == 0) {
        return TOKEN_FUN;
    }
    return TOKEN_IDENTIFIER;
}

Token lexer_next(Lexer *lexer)
{
    skip_trivia(lexer);
    Token token = {
        .offset = lexer->offset,
        .line = lexer->line,
        .column = lexer->column,
    };
    token.kind = scan_token(lexer);
    token.length = lexer->offset - token.offset;
    return token;
}
