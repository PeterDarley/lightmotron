#include "utils.h"

int16_t utils_bytes_to_int(uint8_t first_byte, uint8_t second_byte)
{
    /* Faithful translation of lib/utils.py's bytes_to_int(), including its
     * exact bitwise formula (not a generic two's-complement helper) — the
     * low byte's complement is incremented and then OR'd (not added) into
     * the shifted high byte, matching the Python source line for line. */
    if (!(first_byte & 0x80)) {
        return (int16_t)((first_byte << 8) | second_byte);
    }

    return -(int16_t)(((first_byte ^ 0xFF) << 8) | ((second_byte ^ 0xFF) + 1));
}
