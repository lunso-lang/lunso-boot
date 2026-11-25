#pragma once

#include "ir.h"

typedef struct {
    const char *output;
    const char **objects;
    size_t object_count;
} NativeOptions;

bool native_build(const IrModule *module, const char *source, const NativeOptions *options);
