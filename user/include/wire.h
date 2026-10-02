/* Little-endian fields in a byte buffer, for Jam OS's own wire formats
 * (<updwire.h>, <netlog.h>): what goes over the network is bytes at
 * fixed offsets, never a C struct, so nothing depends on the compiler's
 * layout and a reader never trusts an alignment. The Mac's side reads
 * and writes the same fields with Python's struct "<". */
#pragma once

#include <stddef.h>
#include <stdint.h>

static inline void wire_put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void wire_put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static inline void wire_put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static inline uint16_t wire_get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static inline uint32_t wire_get32(const uint8_t *p)
{
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

static inline uint64_t wire_get64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}
