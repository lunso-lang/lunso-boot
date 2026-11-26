#pragma once

#include <stdbool.h>

#include "ast.h"

typedef enum {
    TYPE_INVALID, TYPE_VOID, TYPE_BOOL,
    TYPE_I8, TYPE_I16, TYPE_I32, TYPE_I64,
    TYPE_U8, TYPE_U16, TYPE_U32, TYPE_U64,
    TYPE_F32, TYPE_F64, TYPE_STRUCT, TYPE_ARRAY, TYPE_SLICE, TYPE_POINTER,
} TypeKind;

typedef struct {
    TypeKind kind;
    uint32_t length;  // Number of elements for fixed arrays.
    TypeKind element;  // Element type for arrays and slices.
    uint32_t element_type;  // Index in Sema.elements for arrays and slices.
    uint32_t element_size;  // Byte size of one element for arrays and slices.
    uint32_t record;       // Struct index, or one-based enum index for an integer representation.
    uint32_t record_size;  // Size including alignment padding.
    uint32_t pointee;      // Index in Sema.pointees for a pointer or a collection of pointers.
    bool read_only;        // The pointer (or a pointer element) gives read-only access.
} SemaType;

typedef enum { BUILTIN_NONE, BUILTIN_LEN, BUILTIN_DATA, BUILTIN_SIZE_OF } SemaBuiltin;

typedef struct {
    SemaType type;
    uint32_t binding;  // Local slot, function index, field byte offset, or enum value index.
    SemaBuiltin builtin;
    TypeKind promotion;  // Numeric destination, or TYPE_INVALID for the original type.
    bool is_boolean_literal;
    bool borrow_array;  // The expression's fixed array is used as a full slice.
    bool falls_through;
    bool is_enum_case;
    bool is_const;
} SemaNode;

typedef struct {
    SemaType result;
    uint32_t slot_count;
    uint32_t declaration;
    SemaNode *nodes;       // Separate annotations for an instantiated function.
    SemaType *arguments;   // Concrete type arguments for an instance.
    AstNodeId instantiated_at;
    unsigned instantiation_depth;
} SemaFunction;

typedef struct {
    SemaType type;
    uint64_t bits;
    AstNodeId string;
    uint8_t state;
} SemaConstant;

typedef struct {
    uint32_t declaration;
    uint32_t size;
    uint32_t alignment;
    uint8_t state;
    bool indirect_fields_resolved;
    SemaNode *fields;
    SemaType *arguments;
    AstNodeId instantiated_at;
    unsigned instantiation_depth;
} SemaRecord;

typedef struct {
    SemaNode *nodes;          // Indexed by AstNodeId.
    SemaFunction *functions;  // Original declarations followed by concrete instances.
    SemaRecord *records;       // Original declarations followed by concrete instances.
    SemaConstant *constants;
    SemaType *pointees;
    SemaType *elements;
    uint64_t *enum_values;
    uint64_t *match_values;
    uint32_t pointee_count;
    uint32_t element_count;
    uint32_t match_value_count;
    uint32_t function_count;
    uint32_t record_count;
    uint32_t constant_count;
} Sema;

typedef struct {
    uint32_t offset;
    const char *message;
    uint32_t instantiation_offset;
    bool has_instantiation;
} SemaError;

static inline SemaNode *sema_function_node(const Sema *sema, const AstFunction *declaration,
                                          const SemaFunction *instance, AstNodeId id)
{
    if (instance->nodes && id >= declaration->nodes.first &&
        id - declaration->nodes.first < declaration->nodes.count) {
        return &instance->nodes[id - declaration->nodes.first];
    }
    return &sema->nodes[id];
}

// Resolve project imports with modules_load before checking a multi-file AST.
bool sema_check(const Ast *ast, Sema *out, SemaError *error);
void sema_destroy(Sema *sema);
const char *sema_type_name(TypeKind type);
uint32_t sema_scalar_size(TypeKind type);
uint32_t sema_element_size(SemaType type);
uint32_t sema_storage_size(SemaType type);
uint32_t sema_type_size(SemaType type);
