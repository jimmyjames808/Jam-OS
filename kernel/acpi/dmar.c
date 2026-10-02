/* The DMAR table's parser (see <jam/dmar.h>): the VT-d remapping units,
 * reserved memory regions, ATS root ports, SoC ATC devices and ACPI
 * namespace devices, with their device scopes (Intel VT-d specification
 * 4.1, chapter 8).
 *
 * The table is firmware input: lengths are checked before every read
 * (a structure inside the table, a scope inside its structure, a path
 * inside its scope), and every field is read with byte copies, since the
 * table's structures are packed and need not be aligned. */
#include <stdbool.h>
#include <jam/dmar.h>
#include <jam/string.h>

/* The DMAR header (VT-d 8.1): the ACPI header, then these, then the
 * remapping structures from byte 48. */
#define DMAR_OFF_HAW     36   /* host address width - 1 */
#define DMAR_OFF_FLAGS   37
#define DMAR_OFF_FIRST   48   /* 10 reserved bytes end at 47 */

/* Every remapping structure starts with a 16-bit type and a 16-bit length. */
#define STRUCT_HDR       4

/* Each structure's fixed part, before its device scopes (VT-d 8.3-8.8). */
#define DRHD_FIXED       16   /* type, length, flags, size, segment, base */
#define RMRR_FIXED       24   /* type, length, reserved, segment, base, limit */
#define ATSR_FIXED       8    /* type, length, flags, reserved, segment (SATC: the same) */
#define RHSA_FIXED       20   /* type, length, reserved, base, proximity domain */
#define ANDD_FIXED       8    /* type, length, 3 reserved, device number, then the name */

#define SCOPE_FIXED      6    /* type, length, flags, reserved, enum id, start bus */

static uint16_t rd16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint64_t rd64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

const char *dmar_type_name(uint32_t type)
{
    switch (type) {
    case DMAR_TYPE_DRHD: return "DRHD";
    case DMAR_TYPE_RMRR: return "RMRR";
    case DMAR_TYPE_ATSR: return "ATSR";
    case DMAR_TYPE_RHSA: return "RHSA";
    case DMAR_TYPE_ANDD: return "ANDD";
    case DMAR_TYPE_SATC: return "SATC";
    case DMAR_TYPE_SIDP: return "SIDP";
    }
    return "unknown";
}

const char *dmar_scope_name(uint32_t type)
{
    switch (type) {
    case DMAR_SCOPE_ENDPOINT: return "endpoint";
    case DMAR_SCOPE_BRIDGE:   return "bridge";
    case DMAR_SCOPE_IOAPIC:   return "IOAPIC";
    case DMAR_SCOPE_HPET:     return "HPET";
    case DMAR_SCOPE_ACPI:     return "ACPI device";
    }
    return "unknown scope";
}

/* One scope at p (len bytes left in its structure) into out->scopes;
 * returns the scope's length, or 0 if it can't be one (the caller stops
 * walking this structure's scopes). */
static uint32_t parse_scope(const uint8_t *p, uint32_t len, struct dmar_info *out,
                            struct dmar_scopes *list)
{
    if (len < SCOPE_FIXED || p[1] < SCOPE_FIXED || p[1] > len || (p[1] - SCOPE_FIXED) % 2)
        return 0;
    if (out->nscopes == DMAR_MAX_SCOPES) {
        out->dropped++;
        return p[1];
    }
    struct dmar_scope *s = &out->scopes[out->nscopes++];
    if (!list->count)
        list->first = (uint16_t)(out->nscopes - 1);
    list->count++;
    s->type = p[0];
    s->enum_id = p[4];
    s->start_bus = p[5];
    uint32_t steps = (p[1] - SCOPE_FIXED) / 2u;
    s->path_full = steps > 255 ? 255 : (uint8_t)steps;
    s->path_len = steps > DMAR_MAX_PATH ? DMAR_MAX_PATH : (uint8_t)steps;
    if (steps > DMAR_MAX_PATH)
        out->dropped++;
    for (uint32_t i = 0; i < s->path_len; i++) {
        s->path[i].dev = p[SCOPE_FIXED + 2 * i];
        s->path[i].fn = p[SCOPE_FIXED + 2 * i + 1];
    }
    return p[1];
}

/* The scopes after a structure's fixed part: [p + fixed, p + len). A
 * scope that doesn't fit ends the list (counted in `dropped`: the
 * structure's own length was fine, so the walk goes on). */
static void parse_scopes(const uint8_t *p, uint32_t len, uint32_t fixed, struct dmar_info *out,
                         struct dmar_scopes *list)
{
    for (uint32_t off = fixed; off < len;) {
        uint32_t n = parse_scope(p + off, len - off, out, list);
        if (!n) {
            out->dropped++;
            return;
        }
        off += n;
    }
}

static void parse_drhd(const uint8_t *p, uint32_t len, struct dmar_info *out)
{
    if (out->nunits == DMAR_MAX_UNITS) {
        out->dropped++;
        return;
    }
    struct dmar_unit *u = &out->units[out->nunits++];
    u->flags = p[4];
    u->size = p[5] & 0x0f;
    u->segment = rd16(p + 6);
    u->base = rd64(p + 8);
    parse_scopes(p, len, DRHD_FIXED, out, &u->scopes);
}

static void parse_rmrr(const uint8_t *p, uint32_t len, struct dmar_info *out)
{
    if (out->nrmrrs == DMAR_MAX_RMRRS) {
        out->dropped++;
        return;
    }
    struct dmar_rmrr *r = &out->rmrrs[out->nrmrrs++];
    r->segment = rd16(p + 6);
    r->base = rd64(p + 8);
    r->limit = rd64(p + 16);
    parse_scopes(p, len, RMRR_FIXED, out, &r->scopes);
}

static void parse_portset(const uint8_t *p, uint32_t len, struct dmar_info *out,
                          struct dmar_portset *set, uint32_t *n, uint32_t max)
{
    if (*n == max) {
        out->dropped++;
        return;
    }
    struct dmar_portset *s = &set[(*n)++];
    s->flags = p[4];
    s->segment = rd16(p + 6);
    parse_scopes(p, len, ATSR_FIXED, out, &s->scopes);
}

static void parse_andd(const uint8_t *p, uint32_t len, struct dmar_info *out)
{
    if (out->nandds == DMAR_MAX_ANDDS) {
        out->dropped++;
        return;
    }
    struct dmar_andd *a = &out->andds[out->nandds++];
    a->dev_num = p[7];
    uint32_t n = 0;
    for (uint32_t i = ANDD_FIXED; i < len && p[i] && n + 1 < DMAR_NAME_MAX; i++)
        a->name[n++] = (p[i] >= 0x20 && p[i] < 0x7f) ? (char)p[i] : '?';
    a->name[n] = '\0';
}

/* The fixed part each known type needs before anything is read from it. */
static uint32_t fixed_len(uint32_t type)
{
    switch (type) {
    case DMAR_TYPE_DRHD: return DRHD_FIXED;
    case DMAR_TYPE_RMRR: return RMRR_FIXED;
    case DMAR_TYPE_ATSR: return ATSR_FIXED;
    case DMAR_TYPE_RHSA: return RHSA_FIXED;
    case DMAR_TYPE_ANDD: return ANDD_FIXED;
    case DMAR_TYPE_SATC: return ATSR_FIXED;
    }
    return STRUCT_HDR;
}

/* One remapping structure of `len` bytes (already checked to fit). */
static void parse_struct(const uint8_t *p, uint32_t len, struct dmar_info *out)
{
    uint32_t type = rd16(p);
    switch (type) {
    case DMAR_TYPE_DRHD: parse_drhd(p, len, out); break;
    case DMAR_TYPE_RMRR: parse_rmrr(p, len, out); break;
    case DMAR_TYPE_ATSR:
        parse_portset(p, len, out, out->atsrs, &out->natsrs, DMAR_MAX_ATSRS);
        break;
    case DMAR_TYPE_SATC:
        parse_portset(p, len, out, out->satcs, &out->nsatcs, DMAR_MAX_SATCS);
        break;
    case DMAR_TYPE_ANDD: parse_andd(p, len, out); break;
    case DMAR_TYPE_RHSA: out->nrhsas++; break;
    default:             out->unknown++; break;
    }
}

static bool header_ok(const uint8_t *t, size_t len)
{
    if (len < DMAR_OFF_FIRST || memcmp(t, "DMAR", 4))
        return false;
    uint32_t tlen = rd32(t + 4);
    if (tlen < DMAR_OFF_FIRST || tlen > len)
        return false;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < tlen; i++)
        sum += t[i];
    return sum == 0;
}

status_t dmar_parse(const void *table, size_t len, struct dmar_info *out)
{
    memset(out, 0, sizeof(*out));
    const uint8_t *t = table;
    if (!t || !header_ok(t, len))
        return ERR_INVALID_ARGS;
    uint32_t tlen = rd32(t + 4);
    out->revision = t[8];
    out->haw = (uint8_t)(t[DMAR_OFF_HAW] + 1);
    out->flags = t[DMAR_OFF_FLAGS];
    for (uint32_t off = DMAR_OFF_FIRST; off < tlen;) {
        uint32_t left = tlen - off;
        uint32_t slen = left >= STRUCT_HDR ? rd16(t + off + 2) : 0;
        if (slen < STRUCT_HDR || slen > left || slen < fixed_len(rd16(t + off))) {
            out->malformed = 1;
            out->malformed_at = off;
            break;
        }
        parse_struct(t + off, slen, out);
        off += slen;
    }
    return OK;
}
