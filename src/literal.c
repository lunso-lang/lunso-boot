#include "literal.h"

static int hex_digit(unsigned char byte)
{
    if (byte >= '0' && byte <= '9') {
        return byte - '0';
    }
    if (byte >= 'a' && byte <= 'f') {
        return byte - 'a' + 10;
    }
    if (byte >= 'A' && byte <= 'F') {
        return byte - 'A' + 10;
    }
    return -1;
}

bool literal_next_byte(const unsigned char *text, uint32_t end, uint32_t *cursor,
                       unsigned char *byte)
{
    uint32_t next = *cursor;
    unsigned char value = text[next++];
    if (value == '\\') {
        if (next == end) {
            return false;
        }
        unsigned char escape = text[next++];
        switch (escape) {
        case '0': value = 0; break;
        case 'n': value = '\n'; break;
        case 'r': value = '\r'; break;
        case 't': value = '\t'; break;
        case '\\':
        case '"':
        case '\'':
            value = escape;
            break;
        case 'x': {
            if (end - next < 2) {
                return false;
            }
            int high = hex_digit(text[next]);
            int low = hex_digit(text[next + 1]);
            if (high < 0 || low < 0) {
                return false;
            }
            value = (unsigned char)(high * 16 + low);
            next += 2;
            break;
        }
        default:
            return false;
        }
    }
    *cursor = next;
    *byte = value;
    return true;
}

bool literal_integer_value(const unsigned char *text, uint32_t length, uint64_t *value)
{
    if (length >= 2 && text[0] == '\'') {
        if (text[length - 1] != '\'') {
            return false;
        }
        uint32_t cursor = 1;
        uint32_t end = length - 1;
        if (cursor >= end) {
            return false;
        }
        unsigned char byte = 0;
        if (!literal_next_byte(text, end, &cursor, &byte) || cursor != end) {
            return false;
        }
        *value = byte;
        return true;
    }

    uint32_t start = 0;
    unsigned base = 10;
    if (length >= 2 && text[0] == '0') {
        if (text[1] == 'x' || text[1] == 'X') {
            base = 16;
            start = 2;
        } else if (text[1] == 'b' || text[1] == 'B') {
            base = 2;
            start = 2;
        }
    }
    if (start >= length) {
        return false;
    }

    uint64_t acc = 0;
    for (uint32_t i = start; i < length; i++) {
        int digit = hex_digit(text[i]);
        if (digit < 0 || (unsigned)digit >= base) {
            return false;
        }
        if (acc > (UINT64_MAX - (unsigned)digit) / base) {
            return false;
        }
        acc = acc * base + (unsigned)digit;
    }
    *value = acc;
    return true;
}
