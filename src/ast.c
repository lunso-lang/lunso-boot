#include "ast_builder.h"

#include <stdlib.h>
#include <string.h>

static uint32_t grown_capacity(uint32_t capacity, uint32_t needed)
{
    if (capacity > UINT32_MAX / 2) {
        return UINT32_MAX;
    }
    uint32_t grown = capacity ? capacity * 2 : 16;
    return grown < needed ? needed : grown;
}

static void *reserve(void *items, uint32_t *capacity, uint32_t needed, size_t item_size)
{
    if (needed <= *capacity) {
        return items;
    }
    uint32_t grown = grown_capacity(*capacity, needed);
    if (grown > SIZE_MAX / item_size) {
        return NULL;
    }
    void *storage = realloc(items, grown * item_size);
    if (storage) {
        *capacity = grown;
    }
    return storage;
}

static bool grow_nodes(AstBuilder *builder, uint32_t needed)
{
    Ast *ast = &builder->ast;
    uint32_t capacity = grown_capacity(builder->node_capacity, needed);
    size_t bytes_per_node = sizeof(*ast->data) + sizeof(*ast->positions) + sizeof(*ast->kinds);
    if (capacity > SIZE_MAX / bytes_per_node) {
        return false;
    }

    unsigned char *storage = malloc(capacity * bytes_per_node);
    if (!storage) {
        return false;
    }
    // Store wider elements first so all three arrays stay naturally aligned.
    AstNodeData *data = (AstNodeData *)storage;
    uint32_t *positions = (uint32_t *)(storage + capacity * sizeof(*data));
    AstKind *kinds = storage + capacity * (sizeof(*data) + sizeof(*positions));
    if (ast->node_count != 0) {
        memcpy(data, ast->data, ast->node_count * sizeof(*data));
        memcpy(positions, ast->positions, ast->node_count * sizeof(*positions));
        memcpy(kinds, ast->kinds, ast->node_count * sizeof(*kinds));
    }

    free(ast->data);
    ast->data = data;
    ast->positions = positions;
    ast->kinds = kinds;
    builder->node_capacity = capacity;
    return true;
}

AstNodeId ast_add_node(AstBuilder *builder, AstKind kind, uint32_t position, AstNodeData data)
{
    Ast *ast = &builder->ast;
    AstNodeId id = ast->node_count ? ast->node_count : 1;
    if (id == UINT32_MAX ||
        (id >= builder->node_capacity && !grow_nodes(builder, id + 1))) {
        return AST_NODE_NONE;
    }
    if (ast->node_count == 0) {
        ast->data[0] = (AstNodeData){0};
        ast->positions[0] = 0;
        ast->kinds[0] = AST_INVALID;
    }
    ast->data[id] = data;
    ast->positions[id] = position;
    ast->kinds[id] = kind;
    ast->node_count = id + 1;
    return id;
}

bool ast_add_function(AstBuilder *builder, AstFunction function, uint32_t *index)
{
    Ast *ast = &builder->ast;
    if (ast->function_count == UINT32_MAX) {
        return false;
    }
    AstFunction *functions = reserve(ast->functions, &builder->function_capacity,
                                     ast->function_count + 1, sizeof(*functions));
    if (!functions) {
        return false;
    }
    ast->functions = functions;
    *index = ast->function_count++;
    ast->functions[*index] = function;
    return true;
}

bool ast_add_if_branches(AstBuilder *builder, AstIfBranches branches, uint32_t *index)
{
    Ast *ast = &builder->ast;
    if (ast->if_branch_count == UINT32_MAX) {
        return false;
    }
    AstIfBranches *items = reserve(ast->if_branches, &builder->if_branch_capacity,
                                   ast->if_branch_count + 1, sizeof(*items));
    if (!items) {
        return false;
    }
    ast->if_branches = items;
    *index = ast->if_branch_count++;
    ast->if_branches[*index] = branches;
    return true;
}

bool ast_push_child(AstBuilder *builder, AstNodeId child)
{
    if (builder->pending_count == UINT32_MAX) {
        return false;
    }
    AstNodeId *items = reserve(builder->pending, &builder->pending_capacity,
                               builder->pending_count + 1, sizeof(*items));
    if (!items) {
        return false;
    }
    builder->pending = items;
    builder->pending[builder->pending_count++] = child;
    return true;
}

bool ast_finish_children(AstBuilder *builder, uint32_t mark, AstNodeRange *range)
{
    Ast *ast = &builder->ast;
    uint32_t count = builder->pending_count - mark;
    *range = (AstNodeRange){.first = ast->child_count, .count = count};
    if (count == 0) {
        return true;
    }
    if (count > UINT32_MAX - ast->child_count) {
        return false;
    }

    uint32_t needed = ast->child_count + count;
    AstNodeId *children = reserve(ast->children, &builder->child_capacity,
                                  needed, sizeof(*children));
    if (!children) {
        return false;
    }
    memcpy(children + ast->child_count, builder->pending + mark, count * sizeof(*children));
    ast->children = children;
    ast->child_count = needed;
    builder->pending_count = mark;
    return true;
}

void ast_destroy(Ast *ast)
{
    free(ast->data);
    free(ast->children);
    free(ast->functions);
    free(ast->if_branches);
    *ast = (Ast){0};
}
