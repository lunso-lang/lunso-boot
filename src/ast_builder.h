#pragma once

#include <stdbool.h>

#include "ast.h"

typedef struct {
    Ast ast;
    uint32_t node_capacity;
    uint32_t child_capacity;
    uint32_t function_capacity;
    uint32_t if_branch_capacity;
    AstNodeId *pending;
    uint32_t pending_count;
    uint32_t pending_capacity;
} AstBuilder;

AstNodeId ast_add_node(AstBuilder *builder, AstKind kind, uint32_t position, AstNodeData data);
bool ast_add_function(AstBuilder *builder, AstFunction function, uint32_t *index);
bool ast_add_if_branches(AstBuilder *builder, AstIfBranches branches, uint32_t *index);
bool ast_push_child(AstBuilder *builder, AstNodeId child);
bool ast_finish_children(AstBuilder *builder, uint32_t mark, AstNodeRange *range);
