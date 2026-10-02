/* <http.h>: a chunked body (RFC 9112 7.1), read a byte of framing at a
 * time by a small state machine, so a body split anywhere across reads
 * decodes the same. Each part is bounded: a size line HTTP_CHUNK_LINE
 * bytes, a size HTTP_LENGTH_MAX, the trailers HTTP_HEAD_MAX; anything
 * else is broken framing, and the caller stops. */
#include <http.h>

/* ---- a chunked body ------------------------------------------------------------------- */

enum {
    CH_SIZE,         /* the size's hex digits */
    CH_SIZE_WS,      /* spaces after them, before a ';' */
    CH_EXT,          /* ";name=value" after them, to the end of the line */
    CH_SIZE_LF,      /* the size line's CR came: its LF */
    CH_DATA,         /* `left` bytes of the chunk */
    CH_DATA_CR,      /* the CRLF after a chunk's bytes */
    CH_DATA_LF,
    CH_TRAILER,      /* trailer lines, to a blank one */
    CH_TRAILER_LF,
    CH_DONE,
};

void http_chunks_init(struct http_chunks *c)
{
    memset(c, 0, sizeof(*c));
    c->state = CH_SIZE;
}

static int hexval(uint8_t b)
{
    if (b >= '0' && b <= '9')
        return b - '0';
    if (b >= 'a' && b <= 'f')
        return b - 'a' + 10;
    if (b >= 'A' && b <= 'F')
        return b - 'A' + 10;
    return -1;
}

/* The size line has ended: a chunk to read, or the trailers. */
static bool size_done(struct http_chunks *c)
{
    if (!c->digits)
        return false;
    c->state = c->left ? CH_DATA : CH_TRAILER;
    c->line = 0;
    return true;
}

/* One byte of the size line: hex digits, spaces, then ';' and the
 * extensions (printable, read past). */
static bool size_byte(struct http_chunks *c, uint8_t b)
{
    if (++c->line > HTTP_CHUNK_LINE)
        return false;
    if (b == '\r') {
        c->state = CH_SIZE_LF;
        return true;
    }
    if (b == '\n')
        return size_done(c);
    if (c->state == CH_EXT)
        return (b >= 0x20 && b < 0x7f) || b == '\t';
    int h = hexval(b);
    if (h >= 0 && c->state == CH_SIZE) {
        if (c->left > (HTTP_LENGTH_MAX - (uint64_t)h) / 16)
            return false;
        c->left = c->left * 16 + (uint64_t)h;
        c->digits = true;
        return true;
    }
    if (b == ' ' || b == '\t')
        c->state = CH_SIZE_WS;
    else if (b == ';')
        c->state = CH_EXT;
    else
        return false;
    return c->digits;
}

/* One byte of the trailers (header lines we read past). */
static bool trailer_byte(struct http_chunks *c, uint8_t b)
{
    if (++c->trailer > HTTP_HEAD_MAX)
        return false;
    if (c->state == CH_TRAILER_LF) {
        if (b != '\n')
            return false;
    } else if (b == '\r') {
        c->state = CH_TRAILER_LF;
        return true;
    } else if (b != '\n') {
        c->line++;
        return b >= 0x20 || b == '\t';
    }
    c->done = !c->line;   /* a blank line ends them */
    c->state = c->done ? CH_DONE : CH_TRAILER;
    c->line = 0;
    return true;
}

/* One framing byte: false if it breaks the framing. */
static bool frame_byte(struct http_chunks *c, uint8_t b)
{
    switch (c->state) {
    case CH_SIZE:
    case CH_SIZE_WS:
    case CH_EXT:
        return size_byte(c, b);
    case CH_SIZE_LF:
        return b == '\n' && size_done(c);
    case CH_DATA_CR:
        if (b == '\n')
            break;
        c->state = CH_DATA_LF;
        return b == '\r';
    case CH_DATA_LF:
        if (b != '\n')
            return false;
        break;
    default:
        return trailer_byte(c, b);
    }
    /* the chunk's CRLF is through: the next size line */
    c->state = CH_SIZE;
    c->left = 0;
    c->line = 0;
    c->digits = false;
    return true;
}

status_t http_chunks_step(struct http_chunks *c, const uint8_t *in, size_t n, size_t *used,
                          size_t *data, size_t *data_n)
{
    *data = *data_n = 0;
    if (c->state == CH_DATA) {
        size_t k = c->left < n ? (size_t)c->left : n;
        c->left -= k;
        if (!c->left)
            c->state = CH_DATA_CR;
        *data_n = k;
        *used = k;
        return OK;
    }
    size_t k = 0;
    while (k < n && c->state != CH_DATA && !c->done) {   /* bounded by n */
        if (!frame_byte(c, in[k++]))
            return ERR_INVALID_ARGS;
    }
    *used = k;
    return OK;
}
