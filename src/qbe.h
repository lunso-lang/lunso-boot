#pragma once

#include "ir.h"

// Emits QBE IL for a checked Lunso module. Returns false if emission fails.
bool qbe_write(FILE *output, const IrModule *module);
