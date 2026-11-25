#pragma once

#include <stdio.h>

#include "ast.h"

typedef struct {
    char *path;
    uint32_t start;
    uint32_t length;
} ModuleFile;

typedef struct {
    Ast ast;
    unsigned char *source;
    ModuleFile *files;
    uint32_t file_count;
} ModuleProgram;

unsigned char *read_source(FILE *file, size_t *length);
// A null std_dir selects the configured standard-library directory.
bool modules_load(const char *path, const unsigned char *source, size_t length,
                  const char *std_dir, ModuleProgram *out);
void modules_destroy(ModuleProgram *program);
void modules_report(const ModuleProgram *program, uint32_t offset, const char *message);
bool modules_output_overlaps(const ModuleProgram *program, const char *output);
