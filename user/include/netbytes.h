/* Big-endian (network order) loads and stores, for code that reads and
 * writes wire formats byte by byte (the DHCP and DNS cores, their tests).
 * They take byte pointers, so nothing is assumed about alignment; the
 * caller has checked that the bytes are inside its buffer.
 *
 * IPv4 addresses are kept in host order everywhere above the wire:
 * 10.2.21.1 is 0x0a021501, so masks and comparisons are plain integer
 * arithmetic. NET_IPV4 builds one from its four numbers. */
#pragma once

#include <stdint.h>

#define NET_IPV4(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

static inline uint16_t net_get16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t net_get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline void net_put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void net_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
