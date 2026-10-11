/* ext: the two CRCs ext4 uses. CRC32C (Castagnoli, polynomial 0x1EDC6F41,
 * bit-reflected 0x82F63B78) for metadata_csum, eight bytes at a time from
 * eight tables ("slicing by 8"), which the first call fills; CRC16 (ARC,
 * polynomial 0x8005, reflected 0xA001) for uninit_bg's group
 * descriptors, a byte at a time.
 *
 * Both run their state with no inversion going in or coming out: ext4
 * starts each checksum from ~0 or from a seed (another checksum) and
 * stores the state as it ends. (The usual CRC32C of a message is
 * ~ext_crc32c(~0, ...): tools/extcheck.c checks the standard value.) The
 * tables are filled once and only read afterwards; the library runs on
 * one thread (ext.h). */
#include "ext.h"

#define CRC32C_POLY 0x82F63B78u
#define CRC16_POLY  0xA001u

static uint32_t c32[8][256];   /* c32[k][b]: b's CRC pushed through k more zero bytes */
static uint16_t c16[256];
static bool filled;

static void fill(void)
{
    for (uint32_t b = 0; b < 256; b++) {
        uint32_t c = b;
        uint16_t s = (uint16_t)b;
        for (int i = 0; i < 8; i++) {
            c = c & 1 ? c >> 1 ^ CRC32C_POLY : c >> 1;
            s = s & 1 ? (uint16_t)(s >> 1 ^ CRC16_POLY) : (uint16_t)(s >> 1);
        }
        c32[0][b] = c;
        c16[b] = s;
    }
    for (uint32_t b = 0; b < 256; b++)
        for (int k = 1; k < 8; k++)
            c32[k][b] = c32[k - 1][b] >> 8 ^ c32[0][c32[k - 1][b] & 0xff];
    filled = true;
}

uint32_t ext_crc32c(uint32_t crc, const void *p, size_t n)
{
    const uint8_t *s = p;
    if (!filled)
        fill();
    for (; n && ((uintptr_t)s & 7); n--)
        crc = c32[0][(crc ^ *s++) & 0xff] ^ crc >> 8;
    for (; n >= 8; n -= 8, s += 8) {
        uint64_t w;
        __builtin_memcpy(&w, s, 8);
        w ^= crc;
        crc = c32[7][w & 0xff] ^ c32[6][w >> 8 & 0xff] ^ c32[5][w >> 16 & 0xff] ^
              c32[4][w >> 24 & 0xff] ^ c32[3][w >> 32 & 0xff] ^ c32[2][w >> 40 & 0xff] ^
              c32[1][w >> 48 & 0xff] ^ c32[0][w >> 56];
    }
    for (; n; n--)
        crc = c32[0][(crc ^ *s++) & 0xff] ^ crc >> 8;
    return crc;
}

uint16_t ext_crc16(uint16_t crc, const void *p, size_t n)
{
    const uint8_t *s = p;
    if (!filled)
        fill();
    for (; n; n--)
        crc = (uint16_t)(c16[(crc ^ *s++) & 0xff] ^ crc >> 8);
    return crc;
}
