#include "ir.h"

#include <inttypes.h>

static void write_span(FILE *output, const IrModule *module, AstSpan span)
{
    fwrite(module->source + span.offset, 1, span.length, output);
}

static void write_module_prefix(FILE *output, uint32_t module)
{
    if (module) {
        fprintf(output, "m%" PRIu32 ".", module);
    }
}

static void write_function_name(FILE *output, const IrModule *module, IrFunction function)
{
    if (!function.external) {
        write_module_prefix(output, function.module);
    }
    write_span(output, module, function.name);
}

static void write_type(FILE *output, const IrModule *module, SemaType type)
{
    TypeKind kind = type.kind;
    if (kind == TYPE_ARRAY) {
        fprintf(output, "[%" PRIu32 "]", type.length);
        kind = type.element;
    } else if (kind == TYPE_SLICE) {
        fputs("[]", output);
        kind = type.element;
    }
    while (kind == TYPE_POINTER) {
        fputc('*', output);
        if (type.read_only) {
            fputs("const ", output);
        }
        type = module->pointees[type.pointee];
        kind = type.kind;
    }
    if (kind == TYPE_STRUCT) {
        IrStructName record = module->struct_names[type.record];
        write_module_prefix(output, record.module);
        write_span(output, module, record.name);
    } else {
        fputs(sema_type_name(kind), output);
    }
}

static void write_value(FILE *output, uint32_t value)
{
    fprintf(output, "%%%" PRIu32, value);
}

static void write_label(FILE *output, uint32_t label)
{
    fprintf(output, "b%" PRIu32, label);
}

static void write_bytes(FILE *output, const IrModule *module, IrInstruction instruction)
{
    const unsigned char *text = module->source + instruction.a;
    for (uint32_t i = 0; i < instruction.b; i++) {
        unsigned char byte = text[i];
        if (byte < 0x20 || byte >= 0x7f) {
            fprintf(output, "\\x%02X", (unsigned int)byte);
        } else {
            fputc(byte, output);
        }
    }
}

static void write_operation(FILE *output, const IrModule *module, IrInstruction instruction)
{
    static const char *const names[] = {
        [IR_NEGATE] = "neg", [IR_LOGICAL_NOT] = "not", [IR_BIT_NOT] = "bit_not",
        [IR_ADD] = "add", [IR_SUBTRACT] = "sub", [IR_MULTIPLY] = "mul",
        [IR_DIVIDE] = "div", [IR_REMAINDER] = "rem",
        [IR_EQUAL] = "eq", [IR_NOT_EQUAL] = "ne", [IR_LESS] = "lt",
        [IR_LESS_EQUAL] = "le", [IR_GREATER] = "gt", [IR_GREATER_EQUAL] = "ge",
        [IR_BIT_AND] = "bit_and", [IR_BIT_OR] = "bit_or", [IR_BIT_XOR] = "bit_xor",
        [IR_SHIFT_LEFT] = "shl", [IR_SHIFT_RIGHT] = "shr",
    };
    fprintf(output, "%s.", names[instruction.op]);
    write_type(output, module, instruction.type);
    fputc(' ', output);
    write_value(output, instruction.a);
    if (instruction.b) {
        fputs(", ", output);
        write_value(output, instruction.b);
    }
}

static void write_instruction(FILE *output, const IrModule *module, IrInstruction instruction)
{
    if (instruction.op == IR_LABEL) {
        write_label(output, instruction.a);
        fputs(":\n", output);
        return;
    }
    fputs("    ", output);
    if (instruction.result) {
        write_value(output, instruction.result);
        fputs(" = ", output);
    }

    switch (instruction.op) {
    case IR_NUMBER:
    case IR_BOOL:
    case IR_BYTES:
    case IR_ZERO:
        fputs("const.", output);
        write_type(output, module, instruction.type);
        fputc(' ', output);
        if (instruction.op == IR_NUMBER) {
            if (instruction.c) {
                fputc('-', output);
            }
            write_span(output, module, (AstSpan){instruction.a, instruction.b});
        } else if (instruction.op == IR_BYTES) {
            write_bytes(output, module, instruction);
        } else if (instruction.type.kind == TYPE_BOOL) {
            fputs(instruction.a ? "true" : "false", output);
        } else {
            fputc('0', output);
        }
        break;
    case IR_LOAD:
    case IR_STORE:
        fputs(instruction.op == IR_LOAD ? "load." : "store.", output);
        write_type(output, module, instruction.type);
        fprintf(output, " $%" PRIu32, instruction.a);
        if (instruction.op == IR_STORE) {
            fputs(", ", output);
            write_value(output, instruction.b);
        }
        break;
    case IR_LOCAL_ADDRESS:
        fputs("local_address.", output);
        write_type(output, module, instruction.type);
        fprintf(output, " $%" PRIu32, instruction.a);
        break;
    case IR_STORAGE_ALLOC:
        fputs("alloc.", output);
        write_type(output, module, instruction.type);
        break;
    case IR_STORAGE_INIT:
        fputs("init.", output);
        write_type(output, module, instruction.type);
        fputc(' ', output);
        write_value(output, instruction.a);
        fputs(", ", output);
        if (instruction.b) {
            write_value(output, instruction.b);
        } else {
            fputc('0', output);
        }
        break;
    case IR_WIDEN_INDEX:
    case IR_LEN:
    case IR_DATA:
    case IR_MEMORY_LOAD:
    case IR_ARRAY_VIEW:
        fputs(instruction.op == IR_LEN ? "len." :
              instruction.op == IR_DATA ? "data." :
              instruction.op == IR_WIDEN_INDEX ? "widen_index." :
              instruction.op == IR_ARRAY_VIEW ? "array_view." : "load_memory.", output);
        write_type(output, module, instruction.type);
        fputc(' ', output);
        write_value(output, instruction.a);
        break;
    case IR_INT_CAST:
        fputs("cast.", output);
        write_type(output, module, instruction.type);
        fprintf(output, " %s ", sema_type_name(instruction.b));
        write_value(output, instruction.a);
        break;
    case IR_FIELD_ADDRESS:
        fputs("field_address.", output);
        write_type(output, module, instruction.type);
        fputc(' ', output);
        write_value(output, instruction.a);
        fprintf(output, ", %" PRIu32, instruction.b);
        break;
    case IR_ELEMENT_ADDRESS:
    case IR_MEMORY_STORE:
    case IR_SLICE:
        fputs(instruction.op == IR_SLICE ? "slice." :
              instruction.op == IR_ELEMENT_ADDRESS ? "element_address." : "store_memory.", output);
        write_type(output, module, instruction.type);
        fputc(' ', output);
        write_value(output, instruction.a);
        fputs(", ", output);
        write_value(output, instruction.b);
        if (instruction.op == IR_SLICE) {
            fputs(", ", output);
            write_value(output, instruction.c);
        }
        break;
    case IR_CALL:
        fputs("call.", output);
        write_type(output, module, instruction.type);
        fputs(" @", output);
        write_function_name(output, module, module->functions[instruction.a]);
        fputc('(', output);
        for (uint32_t i = 0; i < instruction.c; i++) {
            if (i) {
                fputs(", ", output);
            }
            write_value(output, module->arguments[instruction.b + i]);
        }
        fputc(')', output);
        break;
    case IR_JUMP:
        fputs("jump ", output);
        write_label(output, instruction.a);
        break;
    case IR_BRANCH:
        fputs("branch ", output);
        write_value(output, instruction.a);
        fputs(", ", output);
        write_label(output, instruction.b);
        fputs(", ", output);
        write_label(output, instruction.c);
        break;
    case IR_RETURN:
        fputs("return", output);
        if (instruction.a) {
            fputc(' ', output);
            write_value(output, instruction.a);
        }
        break;
    default:
        write_operation(output, module, instruction);
        break;
    }
    fputc('\n', output);
}

bool ir_write(FILE *output, const IrModule *module)
{
    for (uint32_t i = 0; i < module->function_count; i++) {
        IrFunction fn = module->functions[i];
        if (i) {
            fputc('\n', output);
        }
        fputs(fn.external ? "ext fun @" : "fun @", output);
        write_function_name(output, module, fn);
        fputc('(', output);
        for (uint32_t p = 0; p < fn.parameter_count; p++) {
            if (p) {
                fputs(", ", output);
            }
            if (!fn.external) {
                fprintf(output, "$%" PRIu32 ": ", p);
            }
            write_type(output, module, module->parameters[fn.first_parameter + p]);
        }
        fputs("): ", output);
        write_type(output, module, fn.result);
        if (fn.external) {
            fputc('\n', output);
            continue;
        }
        fprintf(output, " [slots %" PRIu32 "] {\n", fn.slot_count);
        for (uint32_t j = 0; j < fn.instruction_count; j++) {
            write_instruction(output, module, module->instructions[fn.first_instruction + j]);
        }
        fputs("}\n", output);
    }
    return !ferror(output);
}
