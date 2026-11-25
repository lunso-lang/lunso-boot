#pragma once

#include "ir.h"

// Emits QBE IL for a checked Lunso module, returning false on an output error.
bool qbe_write(FILE *output, const IrModule *module);
