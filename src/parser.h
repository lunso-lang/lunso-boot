#pragma once

#include <stdbool.h>

#include "ast.h"
#include "lexer.h"
#include "ast_builder.h"

typedef struct {
    Token token;
    const char *message;
} ParseError;

// On success, out receives an AST that borrows source. On failure, out is empty.
// Release the AST with ast_destroy before reusing out.
bool parser_parse(const unsigned char *source, size_t length, Ast *out, ParseError *error);
// Append one file to a project AST. Parse errors are relative to that file.
bool parser_append(AstBuilder *builder, const unsigned char *source, uint32_t length,
                   uint32_t source_base, uint32_t module, AstNodeRange *out, ParseError *error);
