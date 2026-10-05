/* png: a picture as a PNG file for tools/fontpreview.c, with nothing but
 * the C library: 8-bit RGB, no filter, the image data in stored
 * (uncompressed) deflate blocks. Big, and fine for a preview.
 * PNG: RFC 2083 / ISO 15948 (chunks, CRC-32); zlib: RFC 1950 (Adler-32);
 * deflate's stored blocks: RFC 1951 3.2.4. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fontpreview.h"

#define STORED_MAX 65535   /* bytes in one stored block */

static uint32_t crc_table[256];

static uint32_t crc32_of(uint32_t crc, const uint8_t *p, size_t n)
{
    if (!crc_table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    }
    crc = ~crc;
    for (size_t i = 0; i < n; i++)
        crc = crc_table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* One chunk: length, type, data, CRC of type and data. */
static bool chunk(FILE *f, const char *type, const uint8_t *data, size_t n)
{
    uint8_t head[8], tail[4];
    be32(head, (uint32_t)n);
    memcpy(head + 4, type, 4);
    be32(tail, crc32_of(crc32_of(0, head + 4, 4), data, n));
    return fwrite(head, 1, 8, f) == 8 && fwrite(data, 1, n, f) == n && fwrite(tail, 1, 4, f) == 4;
}

/* The raw rows (a filter byte, then RGB) as a zlib stream of stored blocks. */
static uint8_t *zlib_stored(const uint8_t *raw, size_t n, size_t *out_n)
{
    size_t blocks = n / STORED_MAX + 1;
    uint8_t *z = malloc(2 + n + blocks * 5 + 4), *p = z;
    if (!z)
        return NULL;
    *p++ = 0x78;   /* deflate, 32 KiB window */
    *p++ = 0x01;   /* no preset dictionary, lowest level; (0x78 << 8 | 0x01) % 31 == 0 */
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    size_t off = 0;
    do {
        size_t len = n - off < STORED_MAX ? n - off : STORED_MAX;
        *p++ = off + len == n;   /* BFINAL on the last, BTYPE 00: stored */
        p[0] = (uint8_t)len;
        p[1] = (uint8_t)(len >> 8);
        p[2] = (uint8_t)~len;
        p[3] = (uint8_t)(~len >> 8);
        memcpy(p + 4, raw + off, len);
        p += 4 + len;
        off += len;
    } while (off < n);
    be32(p, b << 16 | a);
    *out_n = (size_t)(p + 4 - z);
    return z;
}

bool png_write(const char *path, const uint32_t *px, int w, int h, int stride)
{
    size_t row = 1 + (size_t)w * 3, n = row * (size_t)h, zn = 0;
    uint8_t *raw = malloc(n);
    if (!raw)
        return false;
    for (int y = 0; y < h; y++) {
        uint8_t *r = raw + row * (size_t)y;
        *r++ = 0;   /* filter: none */
        for (int x = 0; x < w; x++) {
            uint32_t c = px[(size_t)y * (size_t)stride + (size_t)x];
            *r++ = (uint8_t)(c >> 16);
            *r++ = (uint8_t)(c >> 8);
            *r++ = (uint8_t)c;
        }
    }
    uint8_t *z = zlib_stored(raw, n, &zn);
    free(raw);
    FILE *f = fopen(path, "wb");
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    memcpy(ihdr + 8, "\x08\x02\x00\x00\x00", 5);   /* 8 bits, RGB, deflate, no filter, no interlace */
    bool ok = z && f && fwrite("\x89PNG\r\n\x1a\n", 1, 8, f) == 8 && chunk(f, "IHDR", ihdr, 13) &&
              chunk(f, "IDAT", z, zn) && chunk(f, "IEND", NULL, 0);
    if (f && fclose(f))
        ok = false;
    free(z);
    if (!ok)
        fprintf(stderr, "png: can't write %s\n", path);
    return ok;
}
