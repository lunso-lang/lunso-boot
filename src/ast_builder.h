#pragma once

#include <stdbool.h>

#include "ast.h"

typedef struct {
    Ast ast;
    uint32_t node_capacity;
    uint32_t child_capacity;
    uint32_t function_capacity;
    uint32_t struct_capacity;
    uint32_t import_capacity;
    uint32_t if_branch_capacity;
    uint32_t for_header_capacity;
    uint32_t variable_capacity;
    AstNodeId *pending;
    uint32_t pending_count;
    uint32_t pending_capacity;
} AstBuilder;

AstNodeId ast_add_node(AstBuilder *builder, AstKind kind, uint32_t position, AstNodeData data);
bool ast_add_function(AstBuilder *builder, AstFunction function, uint32_t *index);
bool ast_add_struct(AstBuilder *builder, AstStruct record, uint32_t *index);
bool ast_add_import(AstBuilder *builder, AstImport item, uint32_t *index);
bool ast_add_if_branches(AstBuilder *builder, AstIfBranches branches, uint32_t *index);
bool ast_add_for_header(AstBuilder *builder, AstForHeader header, uint32_t *index);
bool ast_add_variable(AstBuilder *builder, AstVariable variable, uint32_t *index);
bool ast_push_child(AstBuilder *builder, AstNodeId child);
bool ast_finish_children(AstBuilder *builder, uint32_t mark, AstNodeRange *range);
