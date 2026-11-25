#include "lexer.h"

#include <stdbool.h>
#include <string.h>

Lexer lexer_init(const unsigned char *source, size_t length)
{
    return (Lexer){.source = source, .length = length, .line = 1, .column = 1};
}

static unsigned char peek(const Lexer *lexer)
{
    return lexer->offset < lexer->length ? lexer->source[lexer->offset] : 0;
}

static unsigned char advance(Lexer *lexer)
{
    unsigned char byte = lexer->source[lexer->offset];
    lexer->offset++;
    lexer->column++;
    return byte;
}

static bool match(Lexer *lexer, unsigned char expected)
{
    if (lexer->offset == lexer->length || lexer->source[lexer->offset] != expected) {
        return false;
    }
    advance(lexer);
    return true;
}

static bool is_newline(unsigned char byte)
{
    return byte == '\n' || byte == '\r';
}

static void finish_newline(Lexer *lexer, unsigned char first)
{
    if (first == '\r') {
        match(lexer, '\n');
    }
    lexer->line++;
    lexer->column = 1;
}

static bool is_digit(unsigned char byte)
{
    return byte >= '0' && byte <= '9';
}

static bool is_identifier_start(unsigned char byte)
{
    return (byte >= 'a' && byte <= 'z') ||
           (byte >= 'A' && byte <= 'Z') || byte == '_';
}

static bool is_identifier_continue(unsigned char byte)
{
    return is_identifier_start(byte) || is_digit(byte);
}

static TokenKind scan_identifier(Lexer *lexer, size_t start)
{
    static const struct {
        const char *text;
        TokenKind kind;
    } keywords[] = {
        {"fun", TOKEN_FUN},
        {"ext", TOKEN_EXT},
        {"struct", TOKEN_STRUCT},
        {"as", TOKEN_AS},
        {"const", TOKEN_CONST},
        {"import", TOKEN_IMPORT},
        {"pub", TOKEN_PUB},
        {"priv", TOKEN_PRIV},
        {"return", TOKEN_RETURN},
        {"if", TOKEN_IF},
        {"else", TOKEN_ELSE},
        {"for", TOKEN_FOR},
        {"var", TOKEN_VAR},
    };

    while (is_identifier_continue(peek(lexer))) {
        advance(lexer);
    }
    size_t length = lexer->offset - start;
    for (size_t index = 0; index < sizeof(keywords) / sizeof(keywords[0]); index++) {
        if (length == strlen(keywords[index].text) &&
            memcmp(lexer->source + start, keywords[index].text, length) == 0) {
            return keywords[index].kind;
        }
    }
    return TOKEN_IDENTIFIER;
}

static void scan_digits(Lexer *lexer)
{
    while (is_digit(peek(lexer))) {
        advance(lexer);
    }
}

static TokenKind scan_number(Lexer *lexer)
{
    scan_digits(lexer);
    TokenKind kind = TOKEN_INTEGER;
    if (peek(lexer) == '.' && lexer->length - lexer->offset >= 2 &&
        is_digit(lexer->source[lexer->offset + 1])) {
        advance(lexer);
        scan_digits(lexer);
        kind = TOKEN_FLOAT;
    }

    if (match(lexer, 'e') || match(lexer, 'E')) {
        if (!match(lexer, '+')) {
            match(lexer, '-');
        }
        if (!is_digit(peek(lexer))) {
            return TOKEN_INVALID_NUMBER;
        }
        scan_digits(lexer);
        kind = TOKEN_FLOAT;
    }
    return kind;
}

static TokenKind scan_string(Lexer *lexer)
{
    while (lexer->offset < lexer->length && !is_newline(peek(lexer))) {
        unsigned char byte = advance(lexer);
        if (byte == '"') {
            return TOKEN_STRING;
        }
        if (byte == '\\' && lexer->offset < lexer->length && !is_newline(peek(lexer))) {
            advance(lexer);
        }
    }
    return TOKEN_UNTERMINATED_STRING;
}

static TokenKind scan_slash(Lexer *lexer)
{
    if (match(lexer, '/')) {
        while (lexer->offset < lexer->length && !is_newline(peek(lexer))) {
            advance(lexer);
        }
        return TOKEN_COMMENT;
    }
    if (match(lexer, '*')) {
        while (lexer->offset < lexer->length) {
            unsigned char byte = advance(lexer);
            if (byte == '*' && match(lexer, '/')) {
                return TOKEN_COMMENT;
            }
            if (is_newline(byte)) {
                finish_newline(lexer, byte);
            }
        }
        return TOKEN_UNTERMINATED_COMMENT;
    }
    return match(lexer, '=') ? TOKEN_SLASH_EQUAL : TOKEN_SLASH;
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
    case ',':
        return TOKEN_COMMA;
    case ';':
        return TOKEN_SEMICOLON;
    case '.':
        return match(lexer, '.') ? TOKEN_DOT_DOT : TOKEN_DOT;
    case '{':
        return TOKEN_LEFT_BRACE;
    case '}':
        return TOKEN_RIGHT_BRACE;
    case '[':
        return TOKEN_LEFT_BRACKET;
    case ']':
        return TOKEN_RIGHT_BRACKET;
    case '+':
        return match(lexer, '=') ? TOKEN_PLUS_EQUAL : TOKEN_PLUS;
    case '-':
        return match(lexer, '=') ? TOKEN_MINUS_EQUAL : TOKEN_MINUS;
    case '*':
        return match(lexer, '=') ? TOKEN_STAR_EQUAL : TOKEN_STAR;
    case '/':
        return scan_slash(lexer);
    case '%':
        return match(lexer, '=') ? TOKEN_PERCENT_EQUAL : TOKEN_PERCENT;
    case '=':
        return match(lexer, '=') ? TOKEN_EQUAL_EQUAL : TOKEN_EQUAL;
    case '!':
        return match(lexer, '=') ? TOKEN_BANG_EQUAL : TOKEN_BANG;
    case '<':
        if (match(lexer, '<')) {
            return match(lexer, '=') ? TOKEN_SHIFT_LEFT_EQUAL : TOKEN_SHIFT_LEFT;
        }
        return match(lexer, '=') ? TOKEN_LESS_EQUAL : TOKEN_LESS;
    case '>':
        if (match(lexer, '>')) {
            return match(lexer, '=') ? TOKEN_SHIFT_RIGHT_EQUAL : TOKEN_SHIFT_RIGHT;
        }
        return match(lexer, '=') ? TOKEN_GREATER_EQUAL : TOKEN_GREATER;
    case '&':
        if (match(lexer, '&')) {
            return TOKEN_AMPERSAND_AMPERSAND;
        }
        return match(lexer, '=') ? TOKEN_AMPERSAND_EQUAL : TOKEN_AMPERSAND;
    case '|':
        if (match(lexer, '|')) {
            return TOKEN_PIPE_PIPE;
        }
        return match(lexer, '=') ? TOKEN_PIPE_EQUAL : TOKEN_PIPE;
    case '^':
        return match(lexer, '=') ? TOKEN_CARET_EQUAL : TOKEN_CARET;
    case '~':
        return TOKEN_TILDE;
    case '"':
        return scan_string(lexer);
    case '\r':
    case '\n':
        finish_newline(lexer, byte);
        return TOKEN_NEWLINE;
    default:
        break;
    }

    if (is_identifier_start(byte)) {
        return scan_identifier(lexer, start);
    }
    if (is_digit(byte)) {
        return scan_number(lexer);
    }
    return TOKEN_INVALID;
}

Token lexer_next(Lexer *lexer)
{
    while (peek(lexer) == ' ' || peek(lexer) == '\t') {
        advance(lexer);
    }
    Token token = {
        .offset = lexer->offset,
        .line = lexer->line,
        .column = lexer->column,
    };
    token.kind = scan_token(lexer);
    token.length = lexer->offset - token.offset;
    return token;
}

const char *lexer_error_message(TokenKind kind)
{
    switch (kind) {
    case TOKEN_INVALID:
        return "unexpected byte";
    case TOKEN_INVALID_NUMBER:
        return "invalid exponent in number";
    case TOKEN_UNTERMINATED_STRING:
        return "unterminated string";
    case TOKEN_UNTERMINATED_COMMENT:
        return "unterminated block comment";
    default:
        return NULL;
    }
}
