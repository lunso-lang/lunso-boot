#pragma once

#include <stdbool.h>
#include <stdint.h>

// The cursor starts inside the quotes. End is the offset of the closing quote.
bool literal_next_byte(const unsigned char *text, uint32_t end, uint32_t *cursor,
                       unsigned char *byte);
