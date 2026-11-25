#pragma once

#include <stdbool.h>

#include "ast.h"

typedef enum {
    TYPE_INVALID, TYPE_VOID, TYPE_BOOL,
    TYPE_I8, TYPE_I16, TYPE_I32, TYPE_I64,
    TYPE_U8, TYPE_U16, TYPE_U32, TYPE_U64,
    TYPE_F32, TYPE_F64, TYPE_BYTES,
} TypeKind;

typedef struct {
    TypeKind kind;
    uint32_t length;  // Number of elements for fixed arrays.
} SemaType;

typedef struct {
    SemaType type;
    uint32_t binding;  // Local slot, or function index on AST_CALL.
    bool is_boolean_literal;
    bool falls_through;
} SemaNode;

typedef struct {
    SemaType result;
    uint32_t slot_count;
} SemaFunction;

typedef struct {
    SemaNode *nodes;          // Indexed by AstNodeId.
    SemaFunction *functions;  // Indexed like Ast.functions.
} Sema;

typedef struct {
    uint32_t offset;
    const char *message;
} SemaError;

bool sema_check(const Ast *ast, Sema *out, SemaError *error);
void sema_destroy(Sema *sema);
const char *sema_type_name(TypeKind type);
