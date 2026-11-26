#define _XOPEN_SOURCE 700

#include "modules.h"
#include "parser.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef LUNSO_STD_DIR
#define LUNSO_STD_DIR ""
#endif

typedef enum { UNLOADED, LOADING, LOADED } LoadState;

typedef struct {
    char *name;
    char *file;
    char *directory;
    LoadState state;
    uint32_t parent;
    AstNodeRange declarations;
    AstModuleScope scope;
    uint32_t binding_capacity;
} Module;

typedef struct {
    ModuleProgram *program;
    AstBuilder builder;
    char *root;
    char *std_root;
    Module *modules;
    uint32_t module_count;
    uint32_t module_capacity;
    uint32_t file_capacity;
    uint32_t source_capacity;
    uint32_t qualified_capacity;
    unsigned depth;
} Loader;

#define NO_MODULE UINT32_MAX
enum { MAX_MODULE_DEPTH = 256 };

unsigned char *read_source(FILE *file, size_t *length)
{
    if (fseek(file, 0, SEEK_END) != 0) {
        return NULL;
    }
    long size = ftell(file);
    if (size < 0 || (uintmax_t)size > SIZE_MAX || fseek(file, 0, SEEK_SET) != 0) {
        return NULL;
    }
    *length = (size_t)size;
    unsigned char *source = malloc(*length ? *length : 1);
    if (source && fread(source, 1, *length, file) != *length) {
        free(source);
        return NULL;
    }
    return source;
}

void modules_report(const ModuleProgram *program, uint32_t offset, const char *message)
{
    for (uint32_t i = program->file_count; i > 0; i--) {
        ModuleFile file = program->files[i - 1];
        if (offset < file.start || offset - file.start > file.length) {
            continue;
        }
        size_t line = 1, column = 1;
        for (uint32_t p = file.start; p < offset; p++) {
            unsigned char byte = program->source[p];
            if (byte == '\r' || byte == '\n') {
                if (byte == '\r' && p + 1 < offset && program->source[p + 1] == '\n') {
                    p++;
                }
                line++;
                column = 1;
            } else {
                column++;
            }
        }
        fprintf(stderr, "%s:%zu:%zu: %s\n", file.path, line, column, message);
        return;
    }
    fprintf(stderr, "%s\n", message);
}

static bool error(Loader *loader, uint32_t offset, const char *message)
{
    modules_report(loader->program, offset, message);
    return false;
}

static bool out_of_memory(Loader *loader, uint32_t offset)
{
    return error(loader, offset, "could not allocate module storage");
}

static void *reserve(void *data, uint32_t *capacity, uint32_t count, size_t size)
{
    if (count <= *capacity) {
        return data;
    }
    uint32_t capacity_new = *capacity > UINT32_MAX / 2 ? UINT32_MAX : *capacity * 2;
    if (capacity_new < 16) {
        capacity_new = 16;
    }
    if (capacity_new < count) {
        capacity_new = count;
    }
    if (capacity_new > SIZE_MAX / size) {
        return NULL;
    }
    void *items = realloc(data, capacity_new * size);
    if (items) {
        *capacity = capacity_new;
    }
    return items;
}

static char *concat(const char *a, const char *b, const char *c)
{
    size_t x = strlen(a), y = strlen(b), z = strlen(c);
    if (x > SIZE_MAX - y || x + y > SIZE_MAX - z - 1) {
        return NULL;
    }
    char *text = malloc(x + y + z + 1);
    if (text) {
        memcpy(text, a, x);
        memcpy(text + x, b, y);
        memcpy(text + x + y, c, z + 1);
    }
    return text;
}

static bool is_file(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

static bool is_directory(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static uint32_t add_module(Loader *loader, Module module)
{
    if (loader->module_count == UINT32_MAX) {
        return NO_MODULE;
    }
    Module *modules = reserve(loader->modules, &loader->module_capacity,
                              loader->module_count + 1, sizeof(*modules));
    if (!modules) {
        return NO_MODULE;
    }
    loader->modules = modules;
    uint32_t index = loader->module_count++;
    modules[index] = module;
    return index;
}

static bool is_std_module(const char *name)
{
    return strcmp(name, "std") == 0 || strncmp(name, "std/", 4) == 0;
}

static bool get_module(Loader *loader, const char *name, uint32_t offset, uint32_t *out)
{
    *out = NO_MODULE;
    bool in_std = loader->std_root && is_std_module(name);
    for (uint32_t i = in_std ? 1 : 0; i < loader->module_count; i++) {
        if (!strcmp(loader->modules[i].name, name)) {
            *out = i;
            return true;
        }
    }
    bool std_root = in_std && name[3] == '\0';
    char *directory = in_std ? concat(loader->std_root, "/", std_root ? "" : name + 4)
                             : concat(loader->root, name, "");
    char *file = directory && !std_root ? concat(directory, "", ".lunso") : NULL;
    if (!directory || (!std_root && !file)) {
        free(directory);
        free(file);
        return out_of_memory(loader, offset);
    }
    if (!is_directory(directory)) {
        free(directory);
        directory = NULL;
    }
    if (file && !is_file(file)) {
        free(file);
        file = NULL;
    }
    if (!file && !directory) {
        return true;
    }

    char *copy = strdup(name);
    if (copy) {
        *out = add_module(loader, (Module){.name = copy, .file = file, .directory = directory});
    }
    if (*out == NO_MODULE) {
        free(copy);
        free(file);
        free(directory);
        return out_of_memory(loader, offset);
    }
    return true;
}

static AstSpan item_name(const Ast *ast, AstNodeId id)
{
    return (AstSpan){ast->positions[id], ast->data[id].a};
}

static char *copy_name(const Ast *ast, AstSpan span)
{
    return strndup((const char *)ast->source + span.offset, span.length);
}

static bool same_text(const Ast *ast, AstSpan span, const char *text)
{
    return span.length == strlen(text) &&
           memcmp(ast->source + span.offset, text, span.length) == 0;
}

static char *module_path(const Ast *ast, AstNodeRange path)
{
    size_t size = 1;
    for (uint32_t i = 0; i < path.count; i++) {
        AstSpan part = item_name(ast, ast->children[path.first + i]);
        if (size > SIZE_MAX - (size_t)part.length - 1) {
            return NULL;
        }
        size += (size_t)part.length + 1;
    }
    char *text = malloc(size);
    if (!text) {
        return NULL;
    }
    size_t at = 0;
    for (uint32_t i = 0; i < path.count; i++) {
        if (i) {
            text[at++] = '/';
        }
        AstSpan part = item_name(ast, ast->children[path.first + i]);
        memcpy(text + at, ast->source + part.offset, part.length);
        at += part.length;
    }
    text[at] = '\0';
    return text;
}

static bool add_source(Loader *loader, const char *path, const unsigned char *source,
                       size_t length, uint32_t import_offset, uint32_t *base)
{
    Ast *ast = &loader->builder.ast;
    ModuleProgram *program = loader->program;
    if (length >= UINT32_MAX - ast->source_length || program->file_count == UINT32_MAX) {
        return error(loader, import_offset, "project source is too large");
    }
    uint32_t needed = ast->source_length + (uint32_t)length + 1;
    unsigned char *bytes = reserve(program->source, &loader->source_capacity, needed, 1);
    if (!bytes) {
        return out_of_memory(loader, import_offset);
    }
    program->source = bytes;
    ast->source = bytes;
    ModuleFile *files = reserve(program->files, &loader->file_capacity,
                                program->file_count + 1, sizeof(*files));
    if (!files) {
        return out_of_memory(loader, import_offset);
    }
    program->files = files;
    char *name = strdup(path);
    if (!name) {
        return out_of_memory(loader, import_offset);
    }

    *base = ast->source_length;
    memcpy(bytes + *base, source, length);
    bytes[needed - 1] = '\n';
    ast->source_length = needed;
    files[program->file_count++] = (ModuleFile){name, *base, (uint32_t)length};
    return true;
}

static bool parse_file(Loader *loader, uint32_t module, const unsigned char *source,
                       size_t length, uint32_t offset)
{
    uint32_t base;
    if (!add_source(loader, loader->modules[module].file, source, length, offset, &base)) {
        return false;
    }
    ParseError parse_error;
    if (!parser_append(&loader->builder, loader->program->source + base, (uint32_t)length,
                       base, module, &loader->modules[module].declarations, &parse_error)) {
        char message[80];
        if (parse_error.token.kind == TOKEN_INVALID) {
            snprintf(message, sizeof(message), "unexpected byte 0x%02X",
                     (unsigned)source[parse_error.token.offset]);
            return error(loader, base + (uint32_t)parse_error.token.offset, message);
        }
        return error(loader, base + (uint32_t)parse_error.token.offset, parse_error.message);
    }
    return true;
}

static bool declaration_name(const Ast *ast, AstNodeId id, AstSpan *name,
                             AstBindingKind *kind, bool *public)
{
    uint32_t index = ast->data[id].a;
    if (ast->kinds[id] == AST_FUNCTION) {
        *name = ast->functions[index].name;
        *public = ast->functions[index].public;
        *kind = BIND_FUNCTION;
        return true;
    }
    if (ast->kinds[id] == AST_STRUCT) {
        *name = ast->structs[index].name;
        *public = ast->structs[index].public;
        *kind = BIND_STRUCT;
        return true;
    }
    if (ast->kinds[id] == AST_ENUM) {
        *name = ast->enums[index].name;
        *public = ast->enums[index].public;
        *kind = BIND_ENUM;
        return true;
    }
    if (ast->kinds[id] == AST_CONST && !ast->consts[index].local) {
        *name = ast->consts[index].name;
        *public = ast->consts[index].public;
        *kind = BIND_CONST;
        return true;
    }
    return false;
}

static const AstBinding *binding_named(const Module *module, const char *name)
{
    for (uint32_t i = 0; i < module->scope.count; i++) {
        if (!strcmp(module->scope.bindings[i].name, name)) {
            return &module->scope.bindings[i];
        }
    }
    return NULL;
}

static bool bind(Loader *loader, uint32_t owner, const char *name, AstBindingKind kind,
                 uint32_t index, uint32_t offset)
{
    Module *module = &loader->modules[owner];
    const AstBinding *prior = binding_named(module, name);
    if (prior) {
        if (prior->kind == kind && prior->index == index) {
            return true;
        }
        modules_report(loader->program, offset, "import introduces a conflicting name");
        return error(loader, prior->offset, "name was first imported here");
    }
    const Ast *ast = &loader->builder.ast;
    for (uint32_t i = 0; i < module->declarations.count; i++) {
        AstNodeId declaration = ast->children[module->declarations.first + i];
        AstSpan existing;
        AstBindingKind declaration_kind;
        bool public;
        if (declaration_name(ast, declaration, &existing, &declaration_kind, &public) &&
            same_text(ast, existing, name)) {
            modules_report(loader->program, offset, "import conflicts with a local declaration");
            return error(loader, existing.offset, "name was declared here");
        }
    }
    if (module->scope.count == UINT32_MAX) {
        return out_of_memory(loader, offset);
    }
    AstBinding *bindings = reserve(module->scope.bindings, &module->binding_capacity,
                                   module->scope.count + 1, sizeof(*bindings));
    if (!bindings) {
        return out_of_memory(loader, offset);
    }
    module->scope.bindings = bindings;
    char *copy = strdup(name);
    if (!copy) {
        return out_of_memory(loader, offset);
    }
    bindings[module->scope.count++] = (AstBinding){copy, index, offset, kind};
    return true;
}

static bool bind_span(Loader *loader, uint32_t owner, AstSpan name, AstBindingKind kind,
                      uint32_t index, uint32_t offset)
{
    char *text = copy_name(&loader->builder.ast, name);
    if (!text) {
        return out_of_memory(loader, offset);
    }
    bool ok = bind(loader, owner, text, kind, index, offset);
    free(text);
    return ok;
}

static bool identifier(const char *name, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        char ch = name[i];
        if (ch != '_' && !(ch >= 'A' && ch <= 'Z') && !(ch >= 'a' && ch <= 'z') &&
            !(i != 0 && ch >= '0' && ch <= '9')) {
            return false;
        }
    }
    return length != 0;
}

static bool child_module(Loader *loader, uint32_t parent, const char *name,
                         uint32_t offset, uint32_t *out)
{
    char *path = concat(loader->modules[parent].name, "/", name);
    if (!path) {
        return out_of_memory(loader, offset);
    }
    bool found = get_module(loader, path, offset, out);
    free(path);
    return found;
}

static bool import_children(Loader *loader, uint32_t owner, uint32_t parent, uint32_t offset)
{
    const char *directory = loader->modules[parent].directory;
    if (!directory) {
        return true;
    }
    struct dirent **entries;
    int count = scandir(directory, &entries, NULL, alphasort);
    if (count < 0) {
        return error(loader, offset, "could not open module directory");
    }
    bool ok = true;
    for (int i = 0; i < count && ok; i++) {
        const char *name = entries[i]->d_name;
        size_t length = strlen(name);
        if (length >= 6 && !memcmp(name + length - 6, ".lunso", 6)) {
            length -= 6;
        }
        if (!identifier(name, length)) {
            continue;
        }
        char *part = strndup(name, length);
        uint32_t child;
        if (!part) {
            ok = out_of_memory(loader, offset);
        } else if (!child_module(loader, parent, part, offset, &child)) {
            ok = false;
        } else if (child != NO_MODULE) {
            ok = bind(loader, owner, part, BIND_MODULE, child, offset);
        }
        free(part);
    }
    for (int i = 0; i < count; i++) {
        free(entries[i]);
    }
    free(entries);
    return ok;
}

static bool load_module(Loader *loader, uint32_t module, uint32_t owner, uint32_t offset);

static bool import_all(Loader *loader, uint32_t owner, uint32_t target, uint32_t offset)
{
    const Ast *ast = &loader->builder.ast;
    AstNodeRange declarations = loader->modules[target].declarations;
    for (uint32_t i = 0; i < declarations.count; i++) {
        AstNodeId id = ast->children[declarations.first + i];
        AstSpan name;
        AstBindingKind kind;
        bool public;
        if (declaration_name(ast, id, &name, &kind, &public) && public &&
            !bind_span(loader, owner, name, kind, ast->data[id].a, offset)) {
            return false;
        }
    }
    return import_children(loader, owner, target, offset);
}

static bool import_selected(Loader *loader, uint32_t owner, uint32_t target, AstImport item)
{
    const Ast *ast = &loader->builder.ast;
    AstNodeRange declarations = loader->modules[target].declarations;
    for (uint32_t i = 0; i < item.items.count; i++) {
        AstNodeId id = ast->children[item.items.first + i];
        AstSpan requested = item_name(ast, id);
        AstNodeId alias = ast->data[id].b;
        AstSpan name = alias ? item_name(ast, alias) : requested;
        bool found = false, private = false;
        for (uint32_t j = 0; j < declarations.count; j++) {
            AstNodeId declaration = ast->children[declarations.first + j];
            AstSpan exported;
            AstBindingKind kind;
            bool public;
            if (!declaration_name(ast, declaration, &exported, &kind, &public) ||
                exported.length != requested.length ||
                memcmp(ast->source + exported.offset, ast->source + requested.offset, exported.length)) {
                continue;
            }
            if (public) {
                if (!bind_span(loader, owner, name, kind, ast->data[declaration].a, requested.offset)) {
                    return false;
                }
                found = true;
            } else {
                private = true;
            }
        }
        if (found) {
            continue;
        }
        if (private) {
            return error(loader, requested.offset, "cannot import a private declaration");
        }
        char *part = copy_name(ast, requested);
        if (!part) {
            return out_of_memory(loader, requested.offset);
        }
        uint32_t child;
        bool ok = child_module(loader, target, part, requested.offset, &child);
        free(part);
        if (!ok) {
            return false;
        }
        if (child == NO_MODULE) {
            return error(loader, requested.offset, "name does not exist in imported module");
        }
        if (!bind_span(loader, owner, name, BIND_MODULE, child, requested.offset)) {
            return false;
        }
    }
    return true;
}

static bool load_import(Loader *loader, uint32_t owner, AstNodeId id)
{
    const Ast *ast = &loader->builder.ast;
    uint32_t offset = ast->positions[id];
    AstImport item = ast->imports[ast->data[id].a];
    char *name = module_path(ast, item.path);
    if (!name) {
        return out_of_memory(loader, offset);
    }
    uint32_t target;
    bool ok = get_module(loader, name, offset, &target);
    if (ok && target == NO_MODULE) {
        if (loader->std_root && is_std_module(name)) {
            modules_report(loader->program, offset, "module not found in the standard library");
            fprintf(stderr, "  module %s in %s\n", name, loader->std_root);
        } else {
            modules_report(loader->program, offset, "module not found in project");
            fprintf(stderr, "  module %s\n", name);
        }
        ok = false;
    }
    free(name);
    if (!ok || !load_module(loader, target, owner, offset)) {
        return false;
    }
    switch (item.kind) {
    case IMPORT_ALL:
        return import_all(loader, owner, target, offset);
    case IMPORT_SELECTED:
        return import_selected(loader, owner, target, item);
    case IMPORT_MODULE: {
        AstNodeId last = ast->children[item.path.first + item.path.count - 1];
        AstSpan alias = item.alias.length ? item.alias : item_name(ast, last);
        return bind_span(loader, owner, alias, BIND_MODULE, target, offset);
    }
    }
    return false;
}

static bool add_qualified(Loader *loader, AstQualified reference)
{
    Ast *ast = &loader->builder.ast;
    AstQualified *references = NULL;
    if (ast->qualified_count != UINT32_MAX) {
        references = reserve(ast->qualified, &loader->qualified_capacity, ast->qualified_count + 1,
                             sizeof(*references));
    }
    if (!references) {
        return out_of_memory(loader, ast->positions[reference.node]);
    }
    ast->qualified = references;
    references[ast->qualified_count++] = reference;
    return true;
}

static bool qualify(Loader *loader, uint32_t owner, AstNodeId id, const AstSpan *parts,
                    uint32_t count, AstBindingKind expected)
{
    const Ast *ast = &loader->builder.ast;
    bool optional = expected == BIND_ENUM || expected == BIND_CONST;
    char *first = copy_name(ast, parts[0]);
    if (!first) {
        return out_of_memory(loader, parts[0].offset);
    }
    const AstBinding *binding = binding_named(&loader->modules[owner], first);
    free(first);
    if (!binding || binding->kind != BIND_MODULE) {
        return expected != BIND_STRUCT || error(loader, parts[0].offset, "unknown module name");
    }

    uint32_t target = binding->index;
    for (uint32_t i = 1; i + 1 < count; i++) {
        char *part = copy_name(ast, parts[i]);
        if (!part) {
            return out_of_memory(loader, parts[i].offset);
        }
        bool ok = child_module(loader, target, part, parts[i].offset, &target);
        free(part);
        if (!ok) {
            return false;
        }
        if (target == NO_MODULE) {
            return optional || error(loader, parts[i].offset, "unknown submodule");
        }
    }
    AstSpan requested = parts[count - 1];
    if (!load_module(loader, target, owner, requested.offset)) {
        return false;
    }
    AstNodeRange declarations = loader->modules[target].declarations;
    for (uint32_t i = 0; i < declarations.count; i++) {
        AstNodeId declaration = ast->children[declarations.first + i];
        AstSpan name;
        AstBindingKind kind;
        bool public;
        if (!declaration_name(ast, declaration, &name, &kind, &public) ||
            (kind != expected && !(expected == BIND_STRUCT && kind == BIND_ENUM)) ||
            name.length != requested.length ||
            memcmp(ast->source + name.offset, ast->source + requested.offset, name.length)) {
            continue;
        }
        bool inaccessible = !public && owner != target;
        if (inaccessible && !optional) {
            return error(loader, requested.offset, "cannot access a private declaration");
        }
        return add_qualified(loader, (AstQualified){id, ast->data[declaration].a, kind, inaccessible});
    }
    return optional || error(loader, requested.offset, expected == BIND_STRUCT
                 ? "unknown type in module" : "unknown function in module");
}

static uint32_t member_path(const Ast *ast, AstNodeId id, AstSpan *parts)
{
    uint32_t start = MAX_MODULE_DEPTH;
    while (ast->kinds[id] == AST_MEMBER && start > 1) {
        parts[--start] = (AstSpan){ast->positions[id], ast->data[id].b};
        id = ast->data[id].a;
    }
    if (start == MAX_MODULE_DEPTH || ast->kinds[id] != AST_IDENTIFIER) {
        return MAX_MODULE_DEPTH;
    }
    parts[--start] = item_name(ast, id);
    return start;
}

static bool resolve_references(Loader *loader, uint32_t owner, AstNodeId first, uint32_t end,
                               const bool *member_prefixes)
{
    const Ast *ast = &loader->builder.ast;
    AstSpan parts[MAX_MODULE_DEPTH];
    for (AstNodeId id = first; id < end; id++) {
        AstNodeData node = ast->data[id];
        if (ast->kinds[id] == AST_QUALIFIED_TYPE) {
            if (node.b > MAX_MODULE_DEPTH) {
                return error(loader, ast->positions[id], "qualified path is too deep");
            }
            for (uint32_t i = 0; i < node.b; i++) {
                parts[i] = item_name(ast, ast->children[node.a + i]);
            }
            if (!qualify(loader, owner, id, parts, node.b, BIND_STRUCT)) {
                return false;
            }
        } else if (ast->kinds[id] == AST_CALL ||
                   ast->kinds[id] == AST_GENERIC_TYPE ||
                   (ast->kinds[id] == AST_MEMBER && !member_prefixes[id - first])) {
            bool call = ast->kinds[id] == AST_CALL;
            bool type = ast->kinds[id] == AST_GENERIC_TYPE;
            AstNodeId base = call || type ? ast->children[node.a] : id;
            if (ast->kinds[base] == AST_GENERIC_CALLEE) {
                base = ast->children[ast->data[base].a];
            }
            uint32_t start = member_path(ast, base, parts);
            uint32_t count = MAX_MODULE_DEPTH - start;
            if (call || type) {
                if (count >= 2 && !qualify(loader, owner, id, parts + start, count,
                                           call ? BIND_FUNCTION : BIND_STRUCT)) {
                    return false;
                }
            } else {
                uint32_t before = loader->builder.ast.qualified_count;
                if (count >= 3 && !qualify(loader, owner, id, parts + start, count - 1, BIND_ENUM)) {
                    return false;
                }
                if (loader->builder.ast.qualified_count == before && count >= 2 &&
                    !qualify(loader, owner, id, parts + start, count, BIND_CONST)) {
                    return false;
                }
            }
        }
    }
    return true;
}

static bool cycle(Loader *loader, uint32_t module, uint32_t owner, uint32_t offset)
{
    modules_report(loader->program, offset, "cyclic module import");
    uint32_t chain[MAX_MODULE_DEPTH];
    size_t count = 0;
    while (owner != module && count < MAX_MODULE_DEPTH && owner != NO_MODULE) {
        chain[count++] = owner;
        owner = loader->modules[owner].parent;
    }
    fprintf(stderr, "  %s", loader->modules[module].name);
    while (count) {
        fprintf(stderr, " -> %s", loader->modules[chain[--count]].name);
    }
    fprintf(stderr, " -> %s\n", loader->modules[module].name);
    return false;
}

static bool finish_module(Loader *loader, uint32_t module, AstNodeId first, uint32_t end)
{
    const Ast *ast = &loader->builder.ast;
    AstNodeRange declarations = loader->modules[module].declarations;
    for (uint32_t i = 0; i < declarations.count; i++) {
        AstNodeId id = ast->children[declarations.first + i];
        if (ast->kinds[id] == AST_IMPORT && !load_import(loader, module, id)) {
            return false;
        }
    }
    if (first >= end) {
        return true;
    }
    bool *member_prefixes = calloc(end - first, sizeof(*member_prefixes));
    if (!member_prefixes) {
        return out_of_memory(loader, ast->positions[first]);
    }
    for (AstNodeId id = first; id < end; id++) {
        AstNodeId prefix;
        if (ast->kinds[id] == AST_MEMBER) {
            prefix = ast->data[id].a;
        } else if (ast->kinds[id] == AST_CALL || ast->kinds[id] == AST_GENERIC_TYPE ||
                   ast->kinds[id] == AST_GENERIC_CALLEE) {
            prefix = ast->children[ast->data[id].a];
        } else {
            continue;
        }
        if (ast->kinds[prefix] == AST_MEMBER) {
            member_prefixes[prefix - first] = true;
        }
    }
    bool ok = resolve_references(loader, module, first, end, member_prefixes);
    free(member_prefixes);
    return ok;
}

static bool load_module(Loader *loader, uint32_t module, uint32_t owner, uint32_t offset)
{
    LoadState state = loader->modules[module].state;
    if (state == LOADED) {
        return true;
    }
    if (state == LOADING) {
        return cycle(loader, module, owner, offset);
    }
    if (loader->depth == MAX_MODULE_DEPTH) {
        return error(loader, offset, "module import nesting limit exceeded");
    }
    const char *file = loader->modules[module].file;
    if (!file) {
        loader->modules[module].state = LOADED;
        return true;
    }
    FILE *input = fopen(file, "rb");
    if (!input) {
        perror(file);
        return error(loader, offset, "could not read imported module");
    }
    size_t length;
    unsigned char *source = read_source(input, &length);
    fclose(input);
    if (!source) {
        return error(loader, offset, "could not read imported module");
    }

    loader->depth++;
    loader->modules[module].state = LOADING;
    loader->modules[module].parent = owner;
    uint32_t first = loader->builder.ast.node_count;
    bool parsed = parse_file(loader, module, source, length, offset);
    free(source);
    bool ok = parsed && finish_module(loader, module, first ? first : 1, loader->builder.ast.node_count);
    loader->depth--;
    if (ok) {
        loader->modules[module].state = LOADED;
    }
    return ok;
}

static int compare_qualified(const void *a, const void *b)
{
    AstNodeId x = ((const AstQualified *)a)->node;
    AstNodeId y = ((const AstQualified *)b)->node;
    return (x > y) - (x < y);
}

static bool load_entry(Loader *loader, const char *path, const unsigned char *source, size_t length)
{
    char *file = realpath(path, NULL);
    if (!file) {
        perror(path);
        return false;
    }
    const char *stem = strrchr(file, '/') + 1;
    size_t stem_length = strlen(stem);
    if (stem_length >= 6 && !memcmp(stem + stem_length - 6, ".lunso", 6)) {
        stem_length -= 6;
    }
    loader->root = strndup(file, (size_t)(stem - file));
    char *name = strndup(stem, stem_length);
    free(file);
    char *entry = strdup(path);
    if (!loader->root || !name || !entry ||
        add_module(loader, (Module){.name = name, .file = entry, .state = LOADING,
                                    .parent = NO_MODULE}) == NO_MODULE) {
        free(name);
        free(entry);
        return out_of_memory(loader, 0);
    }
    loader->depth = 1;
    return parse_file(loader, 0, source, length, 0) &&
           finish_module(loader, 0, 1, loader->builder.ast.node_count);
}

static bool select_std_root(Loader *loader, const char *override)
{
    const char *path = override ? override : LUNSO_STD_DIR;
    if (!is_directory(path)) {
        if (!override) {
            return true;
        }
        fprintf(stderr, "%s: --std requires an existing directory\n", path);
        return false;
    }
    loader->std_root = realpath(path, NULL);
    if (!loader->std_root) {
        perror(path);
        return false;
    }
    return true;
}

bool modules_load(const char *path, const unsigned char *source, size_t length,
                  const char *std_dir, ModuleProgram *out)
{
    *out = (ModuleProgram){0};
    Loader loader = {.program = out};
    bool ok = select_std_root(&loader, std_dir) && load_entry(&loader, path, source, length);
    Ast *ast = &loader.builder.ast;
    if (ok) {
        ast->modules = calloc(loader.module_count, sizeof(*ast->modules));
        if (!ast->modules) {
            ok = out_of_memory(&loader, 0);
        }
    }
    if (ok) {
        ast->module_count = loader.module_count;
        ast->declarations = loader.modules[0].declarations;
        if (ast->qualified_count) {
            qsort(ast->qualified, ast->qualified_count, sizeof(*ast->qualified), compare_qualified);
        }
        for (uint32_t i = 0; i < loader.module_count; i++) {
            ast->modules[i] = loader.modules[i].scope;
            loader.modules[i].scope = (AstModuleScope){0};
        }
        out->ast = *ast;
    } else {
        ast_destroy(ast);
        modules_destroy(out);
    }
    for (uint32_t i = 0; i < loader.module_count; i++) {
        Module module = loader.modules[i];
        for (uint32_t j = 0; j < module.scope.count; j++) {
            free(module.scope.bindings[j].name);
        }
        free(module.scope.bindings);
        free(module.name);
        free(module.file);
        free(module.directory);
    }
    free(loader.modules);
    free(loader.root);
    free(loader.std_root);
    free(loader.builder.pending);
    return ok;
}

void modules_destroy(ModuleProgram *program)
{
    ast_destroy(&program->ast);
    for (uint32_t i = 0; i < program->file_count; i++) {
        free(program->files[i].path);
    }
    free(program->files);
    free(program->source);
    *program = (ModuleProgram){0};
}

bool modules_output_overlaps(const ModuleProgram *program, const char *output)
{
    struct stat destination;
    if (stat(output, &destination) != 0) {
        return false;
    }
    for (uint32_t i = 0; i < program->file_count; i++) {
        struct stat input;
        if (stat(program->files[i].path, &input) == 0 &&
            input.st_dev == destination.st_dev && input.st_ino == destination.st_ino) {
            return true;
        }
    }
    return false;
}
