#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>

/**
 * Combine two bytes (big-endian, MSB first) into a signed 16-bit integer.
 *
 * Mirrors lib/utils.py's bytes_to_int(): if the sign bit of first_byte is
 * clear, the result is the plain unsigned big-endian value. If it is set,
 * the result is the two's-complement negative value.
 */
int16_t utils_bytes_to_int(uint8_t first_byte, uint8_t second_byte);

#endif /* UTILS_H */
