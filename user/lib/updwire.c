/* The update protocol's encoder and decoder (<updwire.h>). The decoders
 * take any bytes at all: every field is read once, from the datagram at
 * its fixed offset, and checked before anything is believed; a reply's
 * data pointer is handed out only once its length has been checked
 * against the datagram's. The encoders refuse what the decoders would,
 * so both ends share one set of rules. */
#include <updwire.h>
#include <wire.h>

static bool req_ok(const struct updwire_req *r)
{
    if (r->file >= UPDWIRE_FILES || !r->length || r->length > UPDWIRE_CHUNK_MAX)
        return false;
    return r->snapshot || r->file == UPDWIRE_MANIFEST;
}

/* The reply's own rules: a known status; bytes only with OK, and inside
 * the file; an OK reply names its snapshot. */
static bool rep_ok(const struct updwire_rep *r)
{
    if (r->file >= UPDWIRE_FILES || r->status >= UPDWIRE_STATUSES ||
        r->length > UPDWIRE_CHUNK_MAX)
        return false;
    if (r->status != UPDWIRE_OK)
        return !r->length;
    return r->snapshot && r->offset <= r->file_size && r->length <= r->file_size - r->offset;
}

/* The four bytes every datagram starts with, and its type. */
static void put_head(uint8_t *p, uint8_t type, uint8_t file)
{
    wire_put32(p, UPDWIRE_MAGIC);
    p[4] = UPDWIRE_VERSION;
    p[5] = type;
    p[6] = file;
}

static bool head_ok(const uint8_t *p, uint8_t type)
{
    return wire_get32(p) == UPDWIRE_MAGIC && p[4] == UPDWIRE_VERSION && p[5] == type;
}

status_t updwire_req_encode(const struct updwire_req *r, uint8_t out[UPDWIRE_REQ_SIZE])
{
    if (!req_ok(r))
        return ERR_INVALID_ARGS;
    memset(out, 0, UPDWIRE_REQ_SIZE);
    put_head(out, UPDWIRE_REQUEST, r->file);
    wire_put32(out + 8, r->snapshot);
    wire_put32(out + 12, r->offset);
    wire_put16(out + 16, r->length);
    out[7] = r->format;
    return OK;
}

status_t updwire_req_decode(const void *dgram, size_t len, struct updwire_req *out)
{
    const uint8_t *p = dgram;
    if (!p || len != UPDWIRE_REQ_SIZE || !head_ok(p, UPDWIRE_REQUEST) || wire_get16(p + 18))
        return ERR_INVALID_ARGS;
    struct updwire_req r = {
        .file = p[6], .snapshot = wire_get32(p + 8), .offset = wire_get32(p + 12),
        .length = wire_get16(p + 16), .format = p[7],
    };
    if (!req_ok(&r))
        return ERR_INVALID_ARGS;
    *out = r;
    return OK;
}

status_t updwire_rep_encode(const struct updwire_rep *r, uint8_t *out, size_t cap, size_t *len)
{
    if (!rep_ok(r) || (r->length && !r->data))
        return ERR_INVALID_ARGS;
    size_t n = UPDWIRE_REP_HDR + r->length;
    if (cap < n)
        return ERR_BUFFER_TOO_SMALL;
    memset(out, 0, UPDWIRE_REP_HDR);
    put_head(out, UPDWIRE_REPLY, r->file);
    out[7] = r->status;
    wire_put32(out + 8, r->snapshot);
    wire_put32(out + 12, r->offset);
    wire_put32(out + 16, r->file_size);
    wire_put16(out + 20, r->length);
    if (r->length)
        memcpy(out + UPDWIRE_REP_HDR, r->data, r->length);
    *len = n;
    return OK;
}

status_t updwire_rep_decode(const void *dgram, size_t len, struct updwire_rep *out)
{
    const uint8_t *p = dgram;
    if (!p || len < UPDWIRE_REP_HDR || len > UPDWIRE_REP_MAX || !head_ok(p, UPDWIRE_REPLY) ||
        wire_get16(p + 22))
        return ERR_INVALID_ARGS;
    struct updwire_rep r = {
        .file = p[6], .status = p[7], .snapshot = wire_get32(p + 8),
        .offset = wire_get32(p + 12), .file_size = wire_get32(p + 16),
        .length = wire_get16(p + 20), .data = p + UPDWIRE_REP_HDR,
    };
    if (len != UPDWIRE_REP_HDR + (size_t)r.length || !rep_ok(&r))
        return ERR_INVALID_ARGS;
    *out = r;
    return OK;
}
