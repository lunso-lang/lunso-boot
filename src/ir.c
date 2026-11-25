#include "ir.h"

#include <stdlib.h>

typedef struct {
    const Ast *ast;
    const Sema *sema;
    IrModule *module;
    IrFunction *function;
    uint32_t instruction_capacity;
    uint32_t argument_capacity;
    uint32_t next_value;
    uint32_t next_label;
    bool failed;
} Lowering;

static const IrOp binary_ops[] = {
    [AST_ADD] = IR_ADD, [AST_SUBTRACT] = IR_SUBTRACT,
    [AST_MULTIPLY] = IR_MULTIPLY, [AST_DIVIDE] = IR_DIVIDE,
    [AST_REMAINDER] = IR_REMAINDER,
    [AST_EQUAL] = IR_EQUAL, [AST_NOT_EQUAL] = IR_NOT_EQUAL,
    [AST_LESS] = IR_LESS, [AST_LESS_EQUAL] = IR_LESS_EQUAL,
    [AST_GREATER] = IR_GREATER, [AST_GREATER_EQUAL] = IR_GREATER_EQUAL,
    [AST_BIT_AND] = IR_BIT_AND, [AST_BIT_OR] = IR_BIT_OR, [AST_BIT_XOR] = IR_BIT_XOR,
    [AST_SHIFT_LEFT] = IR_SHIFT_LEFT, [AST_SHIFT_RIGHT] = IR_SHIFT_RIGHT,
    [AST_ASSIGN_ADD] = IR_ADD, [AST_ASSIGN_SUBTRACT] = IR_SUBTRACT,
    [AST_ASSIGN_MULTIPLY] = IR_MULTIPLY, [AST_ASSIGN_DIVIDE] = IR_DIVIDE,
    [AST_ASSIGN_REMAINDER] = IR_REMAINDER,
    [AST_ASSIGN_BIT_AND] = IR_BIT_AND, [AST_ASSIGN_BIT_OR] = IR_BIT_OR,
    [AST_ASSIGN_BIT_XOR] = IR_BIT_XOR,
    [AST_ASSIGN_SHIFT_LEFT] = IR_SHIFT_LEFT, [AST_ASSIGN_SHIFT_RIGHT] = IR_SHIFT_RIGHT,
};

static void *reserve(void *items, uint32_t *capacity, uint32_t needed, size_t item_size)
{
    if (needed <= *capacity) {
        return items;
    }
    uint32_t grown = *capacity > UINT32_MAX / 2 ? UINT32_MAX : *capacity * 2;
    if (grown < 16) {
        grown = 16;
    }
    if (grown < needed) {
        grown = needed;
    }
    if (grown > SIZE_MAX / item_size) {
        return NULL;
    }
    void *result = realloc(items, grown * item_size);
    if (result) {
        *capacity = grown;
    }
    return result;
}

static uint32_t next_id(Lowering *lowering, uint32_t *counter)
{
    if (*counter == UINT32_MAX) {
        lowering->failed = true;
        return 0;
    }
    return (*counter)++;
}

static void emit(Lowering *lowering, IrInstruction instruction)
{
    if (lowering->failed) {
        return;
    }
    IrModule *module = lowering->module;
    if (module->instruction_count == UINT32_MAX) {
        lowering->failed = true;
        return;
    }
    IrInstruction *items = reserve(module->instructions, &lowering->instruction_capacity,
                                   module->instruction_count + 1, sizeof(*items));
    if (!items) {
        lowering->failed = true;
        return;
    }
    module->instructions = items;
    module->instructions[module->instruction_count++] = instruction;
}

static uint32_t emit_value(Lowering *lowering, IrOp op, SemaType type,
                           uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t id = type.kind == TYPE_VOID ? 0 : next_id(lowering, &lowering->next_value);
    emit(lowering, (IrInstruction){op, type, id, a, b, c});
    return id;
}

static uint32_t label(Lowering *lowering)
{
    return next_id(lowering, &lowering->next_label);
}

static void emit_label(Lowering *lowering, uint32_t target)
{
    emit(lowering, (IrInstruction){.op = IR_LABEL, .a = target});
}

static void jump(Lowering *lowering, uint32_t target)
{
    emit(lowering, (IrInstruction){.op = IR_JUMP, .a = target});
}

static void branch(Lowering *lowering, uint32_t condition, uint32_t yes, uint32_t no)
{
    emit(lowering, (IrInstruction){.op = IR_BRANCH, .a = condition, .b = yes, .c = no});
}

static void store(Lowering *lowering, SemaType type, uint32_t slot, uint32_t value)
{
    emit(lowering, (IrInstruction){.op = IR_STORE, .type = type, .a = slot, .b = value});
}

static uint32_t lower_expression(Lowering *lowering, AstNodeId id);
static void lower_statement(Lowering *lowering, AstNodeId id);

static uint32_t lower_index(Lowering *lowering, AstNodeId id)
{
    uint32_t index = lower_expression(lowering, id);
    SemaType type = lowering->sema->nodes[id].type;
    if (type.kind == TYPE_I64 || type.kind == TYPE_U64) {
        return index;
    }
    return emit_value(lowering, IR_WIDEN_INDEX, type, index, 0, 0);
}

static uint32_t lower_element_address(Lowering *lowering, AstNodeId id)
{
    AstNodeData node = lowering->ast->data[id];
    uint32_t base = lower_expression(lowering, node.a);
    uint32_t index = lower_index(lowering, node.b);
    return emit_value(lowering, IR_ELEMENT_ADDRESS, lowering->sema->nodes[node.a].type,
                      base, index, 0);
}

static uint32_t lower_memory_address(Lowering *lowering, AstNodeId id)
{
    AstKind kind = lowering->ast->kinds[id];
    if (kind == AST_INDEX) {
        return lower_element_address(lowering, id);
    }
    AstNodeId base = lowering->ast->data[id].a;
    uint32_t value = lower_expression(lowering, base);
    if (kind == AST_DEREFERENCE) {
        return value;
    }
    return emit_value(lowering, IR_FIELD_ADDRESS, lowering->sema->nodes[base].type,
                      value, lowering->sema->nodes[id].binding, 0);
}

static bool uses_storage(SemaType type)
{
    return type.kind == TYPE_ARRAY || type.kind == TYPE_STRUCT;
}

static void init_storage(Lowering *lowering, SemaType type, uint32_t target, uint32_t source)
{
    emit(lowering, (IrInstruction){.op = IR_STORAGE_INIT, .type = type, .a = target, .b = source});
}

static uint32_t lower_call(Lowering *lowering, AstNodeId id)
{
    const Ast *ast = lowering->ast;
    IrModule *module = lowering->module;
    SemaNode info = lowering->sema->nodes[id];
    AstNodeData call = ast->data[id];
    if (info.builtin != BUILTIN_NONE) {
        AstNodeId argument = ast->children[call.a + 1];
        uint32_t value = lower_expression(lowering, argument);
        IrOp op = info.builtin == BUILTIN_LEN ? IR_LEN : IR_DATA;
        return emit_value(lowering, op, lowering->sema->nodes[argument].type, value, 0, 0);
    }
    uint32_t count = call.b - 1;
    uint32_t first = module->argument_count;
    if (count > UINT32_MAX - first) {
        lowering->failed = true;
        return 0;
    }
    if (count != 0) {
        uint32_t *arguments = reserve(module->arguments, &lowering->argument_capacity,
                                      first + count, sizeof(*arguments));
        if (!arguments) {
            lowering->failed = true;
            return 0;
        }
        module->arguments = arguments;
        module->argument_count = first + count;
    }
    for (uint32_t i = 0; i < count && !lowering->failed; i++) {
        uint32_t value = lower_expression(lowering, ast->children[call.a + 1 + i]);
        // A nested call can relocate the shared argument table.
        module->arguments[first + i] = value;
    }
    return emit_value(lowering, IR_CALL, info.type, info.binding, first, count);
}

static uint32_t lower_logical(Lowering *lowering, AstKind kind, AstNodeData node, SemaType type)
{
    uint32_t left = lower_expression(lowering, node.a);
    uint32_t slot = next_id(lowering, &lowering->function->slot_count);
    uint32_t right_block = label(lowering);
    uint32_t short_block = label(lowering);
    uint32_t end = label(lowering);
    bool is_or = kind == AST_LOGICAL_OR;
    branch(lowering, left, is_or ? short_block : right_block, is_or ? right_block : short_block);

    emit_label(lowering, right_block);
    uint32_t right = lower_expression(lowering, node.b);
    store(lowering, type, slot, right);
    jump(lowering, end);

    emit_label(lowering, short_block);
    uint32_t constant = emit_value(lowering, IR_BOOL, type, is_or, 0, 0);
    store(lowering, type, slot, constant);
    jump(lowering, end);

    emit_label(lowering, end);
    return emit_value(lowering, IR_LOAD, type, slot, 0, 0);
}

static uint32_t lower_binary(Lowering *lowering, AstNodeId id)
{
    AstKind kind = lowering->ast->kinds[id];
    AstNodeData data = lowering->ast->data[id];
    SemaType type = lowering->sema->nodes[id].type;
    if (kind == AST_LOGICAL_AND || kind == AST_LOGICAL_OR) {
        return lower_logical(lowering, kind, data, type);
    }
    if (kind >= AST_ASSIGN && kind <= AST_ASSIGN_SHIFT_RIGHT) {
        AstKind target_kind = lowering->ast->kinds[data.a];
        bool memory = target_kind == AST_INDEX || target_kind == AST_MEMBER ||
                      target_kind == AST_DEREFERENCE;
        uint32_t target = memory ? lower_memory_address(lowering, data.a)
                                 : lowering->sema->nodes[data.a].binding;
        if (uses_storage(type)) {
            uint32_t address = memory ? target : emit_value(lowering, IR_LOAD, type, target, 0, 0);
            uint32_t source = lower_expression(lowering, data.b);
            init_storage(lowering, type, address, source);
            return address;
        }
        uint32_t current = 0;
        if (kind != AST_ASSIGN) {
            current = emit_value(lowering, memory ? IR_MEMORY_LOAD : IR_LOAD, type,
                                 target, 0, 0);
        }
        uint32_t value = lower_expression(lowering, data.b);
        if (kind != AST_ASSIGN) {
            value = emit_value(lowering, binary_ops[kind], type, current, value, 0);
        }
        if (memory) {
            emit(lowering, (IrInstruction){.op = IR_MEMORY_STORE, .type = type,
                                          .a = target, .b = value});
        } else {
            store(lowering, type, target, value);
        }
        return value;
    }

    uint32_t left = lower_expression(lowering, data.a);
    uint32_t right = lower_expression(lowering, data.b);
    // Comparison results are bool. The instruction still needs the operand type.
    SemaNode left_info = lowering->sema->nodes[data.a];
    SemaType operand = left_info.type;
    if (left_info.promotion != TYPE_INVALID) {
        operand.kind = left_info.promotion;
    }
    return emit_value(lowering, binary_ops[kind], operand, left, right, 0);
}

static uint32_t number(Lowering *lowering, AstNodeId id, bool negative)
{
    return emit_value(lowering, IR_NUMBER, lowering->sema->nodes[id].type,
                      lowering->ast->positions[id], lowering->ast->data[id].a, negative);
}

static uint32_t lower_unconverted_expression(Lowering *lowering, AstNodeId id)
{
    if (lowering->failed) {
        return 0;
    }
    const Ast *ast = lowering->ast;
    const SemaNode info = lowering->sema->nodes[id];
    AstNodeData data = ast->data[id];
    AstKind kind = ast->kinds[id];
    switch (kind) {
    case AST_IDENTIFIER:
        return emit_value(lowering, info.is_boolean_literal ? IR_BOOL : IR_LOAD,
                          info.type, info.binding, 0, 0);
    case AST_INTEGER:
    case AST_FLOAT:
        return number(lowering, id, false);
    case AST_STRING:
        return emit_value(lowering, IR_BYTES, info.type, ast->positions[id], data.a, 0);
    case AST_CALL:
        return lower_call(lowering, id);
    case AST_CAST: {
        uint32_t source = lower_expression(lowering, data.a);
        return emit_value(lowering, IR_INT_CAST, info.type, source,
                          lowering->sema->nodes[data.a].type.kind, 0);
    }
    case AST_ADDRESS_OF: {
        if (ast->kinds[data.a] != AST_IDENTIFIER) {
            return lower_memory_address(lowering, data.a);
        }
        SemaNode target = lowering->sema->nodes[data.a];
        if (uses_storage(target.type)) {
            return lower_expression(lowering, data.a);
        }
        return emit_value(lowering, IR_LOCAL_ADDRESS, target.type, target.binding, 0, 0);
    }
    case AST_INDEX:
    case AST_MEMBER:
    case AST_DEREFERENCE: {
        uint32_t address = lower_memory_address(lowering, id);
        return uses_storage(info.type) ? address
                                       : emit_value(lowering, IR_MEMORY_LOAD, info.type, address, 0, 0);
    }
    case AST_SLICE: {
        uint32_t base = lower_expression(lowering, data.a);
        AstNodeData bounds = ast->data[data.b];
        uint32_t start = lower_index(lowering, bounds.a);
        uint32_t end = lower_index(lowering, bounds.b);
        return emit_value(lowering, IR_SLICE, lowering->sema->nodes[data.a].type, base, start, end);
    }
    case AST_POSITIVE:
        return lower_expression(lowering, data.a);
    case AST_NEGATE:
        if (ast->kinds[data.a] == AST_INTEGER) {
            return number(lowering, data.a, true);
        }
        return emit_value(lowering, IR_NEGATE, info.type, lower_expression(lowering, data.a), 0, 0);
    case AST_LOGICAL_NOT:
        return emit_value(lowering, IR_LOGICAL_NOT, info.type,
                          lower_expression(lowering, data.a), 0, 0);
    case AST_BIT_NOT:
        return emit_value(lowering, IR_BIT_NOT, info.type, lower_expression(lowering, data.a), 0, 0);
    default:
        return lower_binary(lowering, id);
    }
}

static uint32_t lower_expression(Lowering *lowering, AstNodeId id)
{
    uint32_t value = lower_unconverted_expression(lowering, id);
    SemaNode info = lowering->sema->nodes[id];
    if (info.borrow_array) {
        return emit_value(lowering, IR_ARRAY_VIEW, info.type, value, 0, 0);
    }
    if (info.promotion != TYPE_INVALID) {
        IrOp op = info.promotion == TYPE_F64 ? IR_FLOAT_EXTEND : IR_INT_CAST;
        return emit_value(lowering, op, (SemaType){.kind = info.promotion},
                          value, info.type.kind, 0);
    }
    return value;
}

static void lower_if(Lowering *lowering, AstNodeId id)
{
    AstNodeData data = lowering->ast->data[id];
    AstIfBranches arms = lowering->ast->if_branches[data.b];
    const SemaNode *nodes = lowering->sema->nodes;
    uint32_t condition = lower_expression(lowering, data.a);
    uint32_t yes = label(lowering);
    uint32_t no = label(lowering);
    uint32_t end = arms.else_branch && nodes[id].falls_through ? label(lowering) : no;
    branch(lowering, condition, yes, no);

    emit_label(lowering, yes);
    lower_statement(lowering, arms.then_branch);
    if (nodes[arms.then_branch].falls_through) {
        jump(lowering, end);
    }
    if (arms.else_branch) {
        emit_label(lowering, no);
        lower_statement(lowering, arms.else_branch);
        if (nodes[arms.else_branch].falls_through) {
            jump(lowering, end);
        }
    }
    if (nodes[id].falls_through) {
        emit_label(lowering, end);
    }
}

static void lower_loop(Lowering *lowering, AstNodeId id)
{
    AstNodeData data = lowering->ast->data[id];
    AstForHeader header = {.condition = data.a};
    if (lowering->ast->kinds[id] == AST_FOR_C_STYLE) {
        header = lowering->ast->for_headers[data.a];
    }
    if (header.initializer) {
        lower_statement(lowering, header.initializer);
    }
    uint32_t start = label(lowering);
    jump(lowering, start);
    emit_label(lowering, start);
    uint32_t end = 0;
    if (header.condition) {
        uint32_t condition = lower_expression(lowering, header.condition);
        uint32_t body = label(lowering);
        end = label(lowering);
        branch(lowering, condition, body, end);
        emit_label(lowering, body);
    }
    lower_statement(lowering, data.b);
    if (lowering->sema->nodes[data.b].falls_through) {
        if (header.step) {
            lower_expression(lowering, header.step);
        }
        jump(lowering, start);
    }
    if (header.condition) {
        emit_label(lowering, end);
    }
}

static void lower_statement(Lowering *lowering, AstNodeId id)
{
    if (lowering->failed) {
        return;
    }
    const Ast *ast = lowering->ast;
    AstNodeData data = ast->data[id];
    switch (ast->kinds[id]) {
    case AST_BLOCK:
        for (uint32_t i = 0; i < data.b && !lowering->failed; i++) {
            AstNodeId child = ast->children[data.a + i];
            lower_statement(lowering, child);
            if (!lowering->sema->nodes[child].falls_through) {
                break;
            }
        }
        break;
    case AST_RETURN: {
        uint32_t value = data.a ? lower_expression(lowering, data.a) : 0;
        emit(lowering, (IrInstruction){.op = IR_RETURN, .a = value});
        break;
    }
    case AST_IF:
        lower_if(lowering, id);
        break;
    case AST_FOR:
    case AST_FOR_C_STYLE:
        lower_loop(lowering, id);
        break;
    case AST_VAR: {
        SemaNode info = lowering->sema->nodes[id];
        AstNodeId initializer = ast->variables[data.a].initializer;
        if (uses_storage(info.type)) {
            uint32_t address = emit_value(lowering, IR_STORAGE_ALLOC, info.type, 0, 0, 0);
            uint32_t from = initializer ? lower_expression(lowering, initializer) : 0;
            init_storage(lowering, info.type, address, from);
            store(lowering, info.type, info.binding, address);
            break;
        }
        uint32_t value = initializer ? lower_expression(lowering, initializer)
                                     : emit_value(lowering, IR_ZERO, info.type, 0, 0, 0);
        store(lowering, info.type, info.binding, value);
        break;
    }
    default:
        lower_expression(lowering, id);
        break;
    }
}

static void lower_function(Lowering *lowering, uint32_t index)
{
    const Ast *ast = lowering->ast;
    IrModule *module = lowering->module;
    AstFunction function = ast->functions[index];
    SemaFunction checked = lowering->sema->functions[index];
    IrFunction *output = &module->functions[index];
    *output = (IrFunction){
        .name = function.name,
        .result = checked.result,
        .first_parameter = module->parameter_count,
        .parameter_count = function.parameters.count,
        .first_instruction = module->instruction_count,
        .slot_count = checked.slot_count,
        .module = function.module,
        .external = function.body == AST_NODE_NONE,
    };
    for (uint32_t i = 0; i < function.parameters.count; i++) {
        AstNodeId param = ast->children[function.parameters.first + i];
        module->parameters[module->parameter_count++] = lowering->sema->nodes[param].type;
    }
    if (output->external) {
        return;
    }

    lowering->function = output;
    lowering->next_value = 1;
    lowering->next_label = 0;
    emit_label(lowering, label(lowering));
    lower_statement(lowering, function.body);
    if (lowering->sema->nodes[function.body].falls_through) {
        emit(lowering, (IrInstruction){.op = IR_RETURN});
    }
    output->instruction_count = module->instruction_count - output->first_instruction;
}

bool ir_lower(const Ast *ast, const Sema *sema, IrModule *out)
{
    *out = (IrModule){.source = ast->source, .function_count = ast->function_count,
                      .module_count = ast->module_count};
    if (ast->struct_count) {
        out->struct_names = calloc(ast->struct_count, sizeof(*out->struct_names));
        if (!out->struct_names) {
            ir_destroy(out);
            return false;
        }
        for (uint32_t i = 0; i < ast->struct_count; i++) {
            out->struct_names[i] = (IrStructName){ast->structs[i].name, ast->structs[i].module};
        }
    }
    if (sema->pointee_count) {
        out->pointees = calloc(sema->pointee_count, sizeof(*out->pointees));
        if (!out->pointees) {
            ir_destroy(out);
            return false;
        }
        for (uint32_t i = 0; i < sema->pointee_count; i++) {
            out->pointees[i] = sema->pointees[i];
        }
    }
    if (ast->function_count == 0) {
        return true;
    }
    uint32_t parameter_count = 0;
    for (uint32_t i = 0; i < ast->function_count; i++) {
        // Every parameter occupies a distinct slot in the AST child table.
        parameter_count += ast->functions[i].parameters.count;
    }
    out->functions = calloc(ast->function_count, sizeof(*out->functions));
    if (parameter_count) {
        out->parameters = calloc(parameter_count, sizeof(*out->parameters));
    }
    if (!out->functions || (parameter_count && !out->parameters)) {
        ir_destroy(out);
        return false;
    }
    Lowering lowering = {.ast = ast, .sema = sema, .module = out};
    for (uint32_t i = 0; i < ast->function_count && !lowering.failed; i++) {
        lower_function(&lowering, i);
    }
    if (lowering.failed) {
        ir_destroy(out);
        return false;
    }
    return true;
}

void ir_destroy(IrModule *module)
{
    free(module->struct_names);
    free(module->pointees);
    free(module->functions);
    free(module->parameters);
    free(module->instructions);
    free(module->arguments);
    *module = (IrModule){0};
}
