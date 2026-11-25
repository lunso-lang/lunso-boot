#pragma once

#include <stdbool.h>

#include "ast.h"
#include "lexer.h"

typedef struct {
    Token token;
    const char *message;
} ParseError;

// On success, out receives an AST that borrows source. On failure, out is empty.
// Release the AST with ast_destroy before reusing out.
bool parser_parse(const unsigned char *source, size_t length, Ast *out, ParseError *error);
