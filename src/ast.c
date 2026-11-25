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

bool ast_add_struct(AstBuilder *builder, AstStruct record, uint32_t *index)
{
    Ast *ast = &builder->ast;
    if (ast->struct_count == UINT32_MAX) {
        return false;
    }
    AstStruct *records = reserve(ast->structs, &builder->struct_capacity,
                                 ast->struct_count + 1, sizeof(*records));
    if (!records) {
        return false;
    }
    ast->structs = records;
    *index = ast->struct_count++;
    ast->structs[*index] = record;
    return true;
}

bool ast_add_import(AstBuilder *builder, AstImport item, uint32_t *index)
{
    Ast *ast = &builder->ast;
    if (ast->import_count == UINT32_MAX) {
        return false;
    }
    AstImport *items = reserve(ast->imports, &builder->import_capacity,
                               ast->import_count + 1, sizeof(*items));
    if (!items) {
        return false;
    }
    ast->imports = items;
    *index = ast->import_count++;
    ast->imports[*index] = item;
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

bool ast_add_for_header(AstBuilder *builder, AstForHeader header, uint32_t *index)
{
    Ast *ast = &builder->ast;
    if (ast->for_header_count == UINT32_MAX) {
        return false;
    }
    AstForHeader *items = reserve(ast->for_headers, &builder->for_header_capacity,
                                  ast->for_header_count + 1, sizeof(*items));
    if (!items) {
        return false;
    }
    ast->for_headers = items;
    *index = ast->for_header_count++;
    ast->for_headers[*index] = header;
    return true;
}

bool ast_add_variable(AstBuilder *builder, AstVariable variable, uint32_t *index)
{
    Ast *ast = &builder->ast;
    if (ast->variable_count == UINT32_MAX) {
        return false;
    }
    AstVariable *items = reserve(ast->variables, &builder->variable_capacity,
                                  ast->variable_count + 1, sizeof(*items));
    if (!items) {
        return false;
    }
    ast->variables = items;
    *index = ast->variable_count++;
    ast->variables[*index] = variable;
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
    free(ast->structs);
    free(ast->imports);
    free(ast->if_branches);
    free(ast->for_headers);
    free(ast->variables);
    for (uint32_t module = 0; module < ast->module_count; module++) {
        AstModuleScope scope = ast->modules[module];
        for (uint32_t i = 0; i < scope.count; i++) {
            free(scope.bindings[i].name);
        }
        free(scope.bindings);
    }
    free(ast->modules);
    free(ast->qualified);
    *ast = (Ast){0};
}

const AstBinding *ast_imported(const Ast *ast, uint32_t module, AstSpan name)
{
    if (module >= ast->module_count) {
        return NULL;
    }
    AstModuleScope scope = ast->modules[module];
    for (uint32_t i = 0; i < scope.count; i++) {
        const AstBinding *binding = &scope.bindings[i];
        if (strlen(binding->name) == name.length &&
            memcmp(ast->source + name.offset, binding->name, name.length) == 0) {
            return binding;
        }
    }
    return NULL;
}

const AstQualified *ast_qualified(const Ast *ast, AstNodeId node)
{
    uint32_t first = 0;
    uint32_t end = ast->qualified_count;
    while (first < end) {
        uint32_t mid = first + (end - first) / 2;
        const AstQualified *item = &ast->qualified[mid];
        if (item->node == node) {
            return item;
        }
        if (item->node < node) {
            first = mid + 1;
        } else {
            end = mid;
        }
    }
    return NULL;
}
