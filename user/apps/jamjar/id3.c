/* jamjar: the cover picture in an MP3's ID3v2 tag (an APIC frame; PIC in
 * ID3v2.2), found in the tag's bytes. Pure: the self-test drives it.
 *
 * The tag (id3.org, ID3v2.3.0 and ID3v2.4.0 structure, ID3v2.2 informal
 * standard): a 10-byte header ("ID3", version, flags, a syncsafe size),
 * maybe an extended header, then frames, then padding (zeros) or, in 2.4,
 * a footer. A frame is an id, a size (2.4: syncsafe; 2.2: three bytes),
 * flags, and its data. APIC's data: a text encoding byte, a MIME type
 * (Latin-1, NUL-ended; PIC: three letters, "PNG" or "JPG"), the picture
 * type (3: the front cover), a description in that encoding (ended by one
 * NUL, or two at an even offset for UTF-16), then the image.
 *
 * Unsynchronisation (a 0x00 put after every 0xFF so no false MPEG sync
 * appears): in 2.2 and 2.3 over the whole tag (the header's flag), in 2.4
 * per frame (its flag, or the header's for all). It is undone in place,
 * which only ever shortens the bytes. Compressed and encrypted frames are
 * passed over. Every length is checked against what is left before it is
 * used: the tag is from a file, so it is untrusted. The image's kind comes
 * from its own first bytes, not from the MIME type. */
#include "jamjar.h"

static uint32_t syncsafe(const uint8_t *p, bool *ok)
{
    *ok &= !((p[0] | p[1] | p[2] | p[3]) & 0x80);
    return (uint32_t)p[0] << 21 | (uint32_t)p[1] << 14 | (uint32_t)p[2] << 7 | p[3];
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

size_t id3_tag_size(const uint8_t h[ID3_HEADER])
{
    bool ok = true;
    if (h[0] != 'I' || h[1] != 'D' || h[2] != '3' || h[3] < 2 || h[3] > 4 || h[4] == 0xff)
        return 0;
    uint32_t n = syncsafe(h + 6, &ok);
    if (!ok)
        return 0;
    return ID3_HEADER + n + (h[3] == 4 && (h[5] & 0x10) ? ID3_HEADER : 0);
}

/* Undo unsynchronisation of p[0..n) in place: FF 00 -> FF. The new length. */
static size_t resync(uint8_t *p, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        p[o++] = p[i];
        if (p[i] == 0xff && i + 1 < n && p[i + 1] == 0x00)
            i++;
    }
    return o;
}

/* The end of a description in encoding enc that starts at p[0..n): the
 * offset just past its terminator, or n + 1 if there is none. */
static size_t text_end(const uint8_t *p, size_t n, uint8_t enc)
{
    if (enc == 1 || enc == 2) {   /* UTF-16: two NULs, on a character boundary */
        for (size_t i = 0; i + 1 < n; i += 2)
            if (!p[i] && !p[i + 1])
                return i + 2;
        return n + 1;
    }
    for (size_t i = 0; i < n; i++)
        if (!p[i])
            return i + 1;
    return n + 1;
}

/* An APIC (or v2.2 PIC) frame's data d[0..n): the picture into *out. */
static bool picture(const uint8_t *d, size_t n, bool v22, struct id3_pic *out)
{
    if (n < 2 || d[0] > 3)
        return false;
    uint8_t enc = d[0];
    size_t at = 1;
    if (v22) {
        at += 3;
    } else {
        size_t m = text_end(d + at, n - at, 0);
        if (m > n - at)
            return false;
        at += m;
    }
    if (at >= n)
        return false;
    uint8_t type = d[at++];
    size_t t = text_end(d + at, n - at, enc);
    if (t > n - at)
        return false;
    at += t;
    const uint8_t *img = d + at;
    size_t len = n - at;
    bool png = len >= 8 && img[0] == 0x89 && img[1] == 'P' && img[2] == 'N' && img[3] == 'G';
    bool jpeg = len >= 4 && img[0] == 0xff && img[1] == 0xd8 && img[2] == 0xff;
    if (!png && !jpeg)
        return false;
    *out = (struct id3_pic){ img, len, type, png, jpeg };
    return true;
}

/* A frame's data, made plain (its flags: grouping byte, length indicator,
 * unsynchronisation); false for one that is compressed or encrypted. */
static bool frame_data(uint8_t ver, const uint8_t *fl, bool tag_unsync, uint8_t **d, size_t *n)
{
    if (ver == 3) {
        if (fl[1] & 0xc0)   /* compressed, encrypted */
            return false;
        if (fl[1] & 0x20) {   /* a group id byte */
            if (*n < 1)
                return false;
            (*d)++;
            (*n)--;
        }
        return true;
    }
    if (fl[1] & 0x0c)   /* compressed, encrypted */
        return false;
    size_t skip = (fl[1] & 0x40 ? 1 : 0) + (fl[1] & 0x01 ? 4 : 0);
    if (*n < skip)
        return false;
    *d += skip;
    *n -= skip;
    if ((fl[1] & 0x02) || tag_unsync)
        *n = resync(*d, *n);
    return true;
}

/* Where the frames start in the tag's body b[0..n) (past an extended
 * header), or n + 1 if the tag can't be read. */
static size_t frames_start(uint8_t ver, uint8_t flags, const uint8_t *b, size_t n)
{
    if (!(flags & 0x40))
        return 0;
    if (ver == 2 || n < 4)
        return n + 1;   /* 2.2: the bit means compression, which no one defined */
    bool ok = true;
    size_t ext = ver == 3 ? (size_t)be32(b) + 4 : syncsafe(b, &ok);
    return ok && ext <= n ? ext : n + 1;
}

bool id3_cover(uint8_t *tag, size_t n, struct id3_pic *out)
{
    if (n < ID3_HEADER || id3_tag_size(tag) == 0 || id3_tag_size(tag) > n)
        return false;
    uint8_t ver = tag[3], flags = tag[5];
    uint8_t *b = tag + ID3_HEADER;
    size_t bn = id3_tag_size(tag) - ID3_HEADER - (ver == 4 && (flags & 0x10) ? ID3_HEADER : 0);
    bool unsync = flags & 0x80;
    if (unsync && ver < 4)
        bn = resync(b, bn);   /* 2.2 and 2.3: the whole tag at once */
    size_t at = frames_start(ver, flags, b, bn), hl = ver == 2 ? 6 : 10;
    bool found = false;
    while (at <= bn && bn - at >= hl && b[at] != 0) {
        const uint8_t *h = b + at;
        bool ok = true;
        size_t sz = ver == 2 ? ((size_t)h[3] << 16 | (size_t)h[4] << 8 | h[5])
                  : ver == 3 ? be32(h + 4) : syncsafe(h + 4, &ok);
        if (!ok || sz > bn - at - hl)
            break;
        bool pic = ver == 2 ? !memcmp(h, "PIC", 3) : !memcmp(h, "APIC", 4);
        uint8_t *d = b + at + hl;
        size_t dn = sz;
        struct id3_pic p;
        if (pic && (ver == 2 || frame_data(ver, h + 8, unsync, &d, &dn)) &&
            picture(d, dn, ver == 2, &p) && (!found || (p.type == 3 && out->type != 3))) {
            *out = p;
            found = true;
        }
        at += hl + sz;
    }
    return found;
}
