#pragma once

#include <stdio.h>

#include "sema.h"

typedef enum {
    IR_LABEL, IR_NUMBER, IR_BOOL, IR_BYTES, IR_ZERO, IR_LOAD, IR_STORE, IR_CALL,
    IR_STORAGE_ALLOC, IR_STORAGE_INIT, IR_WIDEN_INDEX, IR_ELEMENT_ADDRESS,
    IR_FIELD_ADDRESS, IR_MEMORY_LOAD, IR_MEMORY_STORE, IR_SLICE, IR_ARRAY_VIEW, IR_LEN,
    IR_INT_CAST,
    IR_JUMP, IR_BRANCH, IR_RETURN,
    IR_NEGATE, IR_LOGICAL_NOT, IR_BIT_NOT,
    IR_ADD, IR_SUBTRACT, IR_MULTIPLY, IR_DIVIDE, IR_REMAINDER,
    IR_EQUAL, IR_NOT_EQUAL, IR_LESS, IR_LESS_EQUAL, IR_GREATER, IR_GREATER_EQUAL,
    IR_BIT_AND, IR_BIT_OR, IR_BIT_XOR, IR_SHIFT_LEFT, IR_SHIFT_RIGHT,
} IrOp;

// Value IDs begin at 1. Value ID 0 is absent. Slots and labels start at 0.
// Number/byte constants use a source offset and length in a and b. For a number,
// c adds a minus sign. Array operations carry the collection type. Memory
// access uses the scalar type, and field addresses carry their byte offset in b.
// Bounds are widened before use.
// An integer cast stores the destination type and the source type kind in b.
// A call stores a function index, the first argument index, and the argument count.
// Comparison instructions use the operand type and produce a bool value.
typedef struct {
    IrOp op;
    SemaType type;
    uint32_t result;
    uint32_t a, b, c;
} IrInstruction;

typedef struct {
    AstSpan name;
    SemaType result;
    uint32_t first_parameter, parameter_count;
    uint32_t first_instruction, instruction_count;
    uint32_t slot_count;
    bool external;
} IrFunction;

typedef struct {
    const unsigned char *source;  // Borrowed for names and literal spellings.
    AstSpan *struct_names;        // Indexed by SemaType.record.
    IrFunction *functions;
    uint32_t function_count;
    SemaType *parameters;
    uint32_t parameter_count;
    IrInstruction *instructions;
    uint32_t instruction_count;
    uint32_t *arguments;
    uint32_t argument_count;
} IrModule;

// Requires successful semantic analysis of ast. The resulting IR borrows ast->source.
bool ir_lower(const Ast *ast, const Sema *sema, IrModule *out);
void ir_destroy(IrModule *module);
bool ir_write(FILE *output, const IrModule *module);
