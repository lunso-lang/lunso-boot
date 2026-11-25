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
