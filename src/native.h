#pragma once

#include "ir.h"

typedef enum {
    NATIVE_EXE,
    NATIVE_OBJ,
    NATIVE_ASM,
} NativeKind;

typedef struct {
    NativeKind kind;
    bool nostdlib;
    const char *output;
    const char **objects;
    size_t object_count;
} NativeOptions;

bool native_build(const IrModule *module, const char *source, const NativeOptions *options);
int native_run(const char *program, int argc, char *const *argv);
