/* libjwl's codec (<jwl.h>): one Wayland message checked against its
 * signature and decoded, or encoded; the one parser both the compositor
 * and every client use, so it is the one to review and fuzz.
 *
 * Decoding never trusts a length: every word is read only after checking
 * it lies inside the message, and the message only after checking it lies
 * inside the batch, with subtractions that can't wrap (left = end - p,
 * never p + n > end). Decoding is two passes over one message: the first
 * checks every argument and collects the values (touching nothing), the
 * second enters the new ids in the map; so a refused message leaves the
 * batch cursor where it was. The error codes follow libwayland's choices
 * where it has one: a malformed message is invalid_method on the object
 * it was sent to, an id that names nothing or the wrong thing is
 * invalid_object, a full map is no_memory on the display. */
#include <jwl.h>
#include <os.h>

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static void wr32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

/* A string or array's bytes rounded up to whole words. len <= 4096 here. */
static uint32_t padded(uint32_t len)
{
    return (len + 3u) & ~3u;
}

/* ---- signatures and tables ---------------------------------------------------------- */

status_t jwl_sig_parse(const char *s, struct jwl_sig *out)
{
    struct jwl_sig sig = { .since = 0 };
    if (!s)
        return ERR_INVALID_ARGS;
    const char *digits = s;
    for (; *s >= '0' && *s <= '9'; s++) {
        if (sig.since > 100000)
            return ERR_INVALID_ARGS;
        sig.since = sig.since * 10 + (uint32_t)(*s - '0');
    }
    if (s != digits && sig.since == 0)
        return ERR_INVALID_ARGS;   /* "0..." names no version */
    if (sig.since == 0)
        sig.since = 1;
    bool nullable = false;
    for (; *s; s++) {
        if (*s == '?') {
            if (nullable)
                return ERR_INVALID_ARGS;
            nullable = true;
            continue;
        }
        if (!strchr("iufsonah", *s) || sig.n == JWL_ARGS_MAX)
            return ERR_INVALID_ARGS;
        if (nullable && *s != 's' && *s != 'o')
            return ERR_INVALID_ARGS;
        sig.type[sig.n] = *s;
        sig.nullable[sig.n++] = nullable;
        nullable = false;
    }
    if (nullable)   /* a '?' with no letter after it */
        return ERR_INVALID_ARGS;
    *out = sig;
    return OK;
}

/* An 'n' with no interface of its own takes it from the "su" before it. */
static bool untyped_new_id(const struct jwl_message *m, const struct jwl_sig *sig, unsigned i)
{
    return sig->type[i] == 'n' && (!m->types || !m->types[i]);
}

/* One message of a table: its name, signature and since, a type only on
 * an 'o' or 'n' (and a whole one), an untyped 'n' right after a non-null
 * 's' and a 'u'. */
static status_t check_message(const struct jwl_message *m, uint32_t version)
{
    struct jwl_sig sig;
    if (!m->name || jwl_sig_parse(m->signature, &sig) != OK || sig.since > version)
        return ERR_INVALID_ARGS;
    for (unsigned i = 0; i < sig.n; i++) {
        const struct jwl_interface *t = m->types ? m->types[i] : NULL;
        if (t && sig.type[i] != 'o' && sig.type[i] != 'n')
            return ERR_INVALID_ARGS;
        if (t && (!t->name || !t->version))
            return ERR_INVALID_ARGS;
        if (untyped_new_id(m, &sig, i) &&
            (i < 2 || sig.type[i - 2] != 's' || sig.nullable[i - 2] || sig.type[i - 1] != 'u'))
            return ERR_INVALID_ARGS;
    }
    return OK;
}

/* Destructor bits only for messages that exist. */
static bool mask_fits(uint32_t mask, unsigned n)
{
    return n >= 32 || (mask >> n) == 0;
}

status_t jwl_interface_check(const struct jwl_interface *iface)
{
    if (!iface || !iface->name || !iface->version)
        return ERR_INVALID_ARGS;
    if ((iface->nrequests && !iface->requests) || (iface->nevents && !iface->events))
        return ERR_INVALID_ARGS;
    if (!mask_fits(iface->request_destructors, iface->nrequests) ||
        !mask_fits(iface->event_destructors, iface->nevents))
        return ERR_INVALID_ARGS;
    for (unsigned i = 0; i < iface->nrequests; i++)
        if (check_message(&iface->requests[i], iface->version) != OK)
            return ERR_INVALID_ARGS;
    for (unsigned i = 0; i < iface->nevents; i++)
        if (check_message(&iface->events[i], iface->version) != OK)
            return ERR_INVALID_ARGS;
    return OK;
}

bool jwl_interface_same(const struct jwl_interface *a, const struct jwl_interface *b)
{
    return a == b || (a && b && !strcmp(a->name, b->name));
}

void jwl_msg_close_handles(struct jwl_msg *m)
{
    struct jwl_sig sig;
    if (!m->msg || jwl_sig_parse(m->msg->signature, &sig) != OK)
        return;
    for (unsigned i = 0; i < sig.n && i < m->nargs; i++)
        if (sig.type[i] == 'h' && m->args[i].h != HANDLE_INVALID) {
            jam_handle_close(m->args[i].h);
            m->args[i].h = HANDLE_INVALID;
        }
    m->nhandles = 0;
}

/* ---- decoding ----------------------------------------------------------------------- */

/* One message being decoded. */
struct dec {
    struct jwl_map   *map;
    struct jwl_error *err;
    const uint8_t    *p, *end;          /* the arguments not read yet */
    const struct jwl_in *in;
    unsigned          hat;              /* the next handle of the batch */
    uint32_t          id;               /* the object it was sent on */
    const char       *where;            /* "interface.message" for the text */
    uint32_t          version;          /* that object's */
    unsigned          nnew;             /* new ids so far, and what they will be */
    uint32_t          new_ids[JWL_ARGS_MAX];
    const struct jwl_interface *new_iface[JWL_ARGS_MAX];
    uint32_t          new_version[JWL_ARGS_MAX];
};

static status_t fail(struct jwl_error *err, uint32_t object, uint32_t code, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static status_t fail(struct jwl_error *err, uint32_t object, uint32_t code, const char *fmt, ...)
{
    err->object = object;
    err->code = code;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err->text, sizeof(err->text), fmt, ap);
    va_end(ap);
    return ERR_INVALID_ARGS;
}

static status_t bad(struct dec *d, const char *what)
{
    return fail(d->err, d->id, JWL_ERROR_INVALID_METHOD, "%s@%u: %s", d->where, d->id, what);
}

static status_t word(struct dec *d, uint32_t *v)
{
    if (d->end - d->p < 4)
        return bad(d, "arguments run past the message");
    *v = rd32(d->p);
    d->p += 4;
    return OK;
}

/* A string's or array's length word and bytes: *at the bytes, *len. */
static status_t bytes(struct dec *d, const uint8_t **at, uint32_t *len)
{
    status_t st = word(d, len);
    if (st != OK)
        return st;
    if (*len > JWL_STRING_MAX)
        return bad(d, "a string or array over 4096 bytes");
    if ((size_t)(d->end - d->p) < padded(*len))
        return bad(d, "a string or array runs past the message");
    *at = d->p;
    d->p += padded(*len);
    return OK;
}

static status_t dec_string(struct dec *d, bool nullable, const char **out)
{
    const uint8_t *s;
    uint32_t len;
    status_t st = bytes(d, &s, &len);
    if (st != OK)
        return st;
    if (len == 0) {
        if (!nullable)
            return bad(d, "a null string where none is allowed");
        *out = NULL;
        return OK;
    }
    if (s[len - 1] != 0)
        return bad(d, "a string without its NUL");
    for (uint32_t i = 0; i + 1 < len; i++)
        if (s[i] == 0)
            return bad(d, "a NUL inside a string");
    *out = (const char *)s;
    return OK;
}

static status_t dec_object(struct dec *d, bool nullable, const struct jwl_interface *type,
                           uint32_t *out)
{
    uint32_t id;
    status_t st = word(d, &id);
    if (st != OK)
        return st;
    if (id == 0) {
        if (!nullable)
            return bad(d, "a null object where none is allowed");
        *out = 0;
        return OK;
    }
    const struct jwl_object *o = jwl_map_entry(d->map, id);
    if (o && o->state == JWL_ZOMBIE && d->map->side == JWL_CLIENT) {
        *out = 0;   /* one we destroyed: the event raced it */
        return OK;
    }
    if (!o || o->state != JWL_LIVE)
        return fail(d->err, d->id, JWL_ERROR_INVALID_OBJECT, "%s@%u: invalid object %u",
                    d->where, d->id, id);
    if (type && !jwl_interface_same(o->iface, type))
        return fail(d->err, d->id, JWL_ERROR_INVALID_OBJECT, "%s@%u: object %u is a %s, not a %s",
                    d->where, d->id, id, o->iface->name, type->name);
    *out = id;
    return OK;
}

/* A new id the peer made, for an object of iface at version: in its
 * range, free or next, and not twice in one message. */
static status_t dec_new_id(struct dec *d, const struct jwl_interface *iface, uint32_t version,
                           uint32_t *out)
{
    uint32_t id;
    status_t st = word(d, &id);
    if (st != OK)
        return st;
    for (unsigned i = 0; i < d->nnew; i++)
        if (d->new_ids[i] == id)
            st = ERR_INVALID_ARGS;
    if (st == OK)
        st = jwl_map_check_new(d->map, id);
    if (st == ERR_NO_RESOURCES)
        return fail(d->err, JWL_DISPLAY_ID, JWL_ERROR_NO_MEMORY, "%s@%u: no room for object %u",
                    d->where, d->id, id);
    if (st != OK)
        return fail(d->err, d->id, JWL_ERROR_INVALID_OBJECT, "%s@%u: invalid new id %u",
                    d->where, d->id, id);
    d->new_ids[d->nnew] = id;
    d->new_iface[d->nnew] = iface;
    d->new_version[d->nnew++] = version;
    *out = id;
    return OK;
}

/* The untyped new_id: its interface is the one the string before it names
 * (among the connection's known), its version the uint before it. */
static status_t dec_new_any(struct dec *d, const char *name, uint32_t version, uint32_t *out)
{
    const struct jwl_interface *iface = NULL;
    for (unsigned i = 0; i < d->map->nknown && !iface; i++)
        if (!strcmp(d->map->known[i]->name, name))
            iface = d->map->known[i];
    if (!iface)
        return fail(d->err, d->id, JWL_ERROR_INVALID_OBJECT, "%s@%u: unknown interface %.40s",
                    d->where, d->id, name);
    if (version == 0 || version > iface->version)
        return fail(d->err, d->id, JWL_ERROR_INVALID_OBJECT, "%s@%u: %s version %u (1 to %u)",
                    d->where, d->id, iface->name, version, iface->version);
    return dec_new_id(d, iface, version, out);
}

/* Argument i of the message into args[i] (the ones before it decoded). */
static status_t dec_arg(struct dec *d, const struct jwl_sig *sig, unsigned i,
                        const struct jwl_interface *const *types, union jwl_arg *args)
{
    const struct jwl_interface *t = types ? types[i] : NULL;
    union jwl_arg *a = &args[i];
    switch (sig->type[i]) {
    case 'i':
    case 'u':
    case 'f':
        return word(d, &a->u);
    case 's':
        return dec_string(d, sig->nullable[i], &a->s);
    case 'o':
        return dec_object(d, sig->nullable[i], t, &a->o);
    case 'n':
        if (t)
            return dec_new_id(d, t, d->version, &a->n);
        if (i < 2 || sig->type[i - 2] != 's' || sig->type[i - 1] != 'u' || !args[i - 2].s)
            break;   /* jwl_interface_check refuses such a table */
        return dec_new_any(d, args[i - 2].s, args[i - 1].u, &a->n);
    case 'a': {
        const uint8_t *at;
        status_t st = bytes(d, &at, &a->a.size);
        a->a.data = st == OK && a->a.size ? at : NULL;
        return st;
    }
    case 'h':
        if (d->hat >= d->in->nh)
            return bad(d, "an fd argument with no handle left in the batch");
        a->h = d->in->h[d->hat++];
        return OK;
    }
    return fail(d->err, d->id, JWL_ERROR_IMPLEMENTATION, "%s: a broken table", d->where);
}

/* The object a message is for, and which of its messages it is. */
static status_t dec_target(struct jwl_map *map, uint32_t id, uint16_t opcode,
                           struct jwl_msg *out, struct jwl_error *err)
{
    const struct jwl_object *o = jwl_map_entry(map, id);
    bool zombie = o && o->state == JWL_ZOMBIE && map->side == JWL_CLIENT;
    if (!o || (o->state != JWL_LIVE && !zombie))
        return fail(err, JWL_DISPLAY_ID, JWL_ERROR_INVALID_OBJECT, "invalid object %u", id);
    const struct jwl_interface *iface = o->iface;
    bool server = map->side == JWL_SERVER;
    unsigned count = server ? iface->nrequests : iface->nevents;
    if (opcode >= count)
        return fail(err, id, JWL_ERROR_INVALID_METHOD, "%s@%u: no %s %u", iface->name, id,
                    server ? "request" : "event", opcode);
    out->id = id;
    out->opcode = opcode;
    out->iface = iface;
    out->msg = &(server ? iface->requests : iface->events)[opcode];
    out->data = o->data;
    out->version = o->version;
    out->dead_target = zombie;
    return OK;
}

/* Pass 2: the message is good; its new ids join the map (as zombies for a
 * message to a zombie, so what is later sent to them is dropped too). */
static status_t commit_new_ids(struct dec *d, bool dead_target)
{
    for (unsigned i = 0; i < d->nnew; i++) {
        enum jwl_state state = dead_target ? JWL_ZOMBIE : JWL_LIVE;
        if (jwl_map_insert(d->map, d->new_ids[i], d->new_iface[i], d->new_version[i], state) != OK)
            return fail(d->err, JWL_DISPLAY_ID, JWL_ERROR_NO_MEMORY, "no memory for object %u",
                        d->new_ids[i]);
    }
    return OK;
}

status_t jwl_decode(struct jwl_in *in, struct jwl_map *map, struct jwl_msg *out,
                    struct jwl_error *err)
{
    size_t left = in->len - in->at;
    if (left == 0)
        return ERR_SHOULD_WAIT;
    if (left < 8)
        return fail(err, JWL_DISPLAY_ID, JWL_ERROR_INVALID_METHOD, "%u stray bytes", (unsigned)left);
    const uint8_t *p = in->buf + in->at;
    uint32_t id = rd32(p), size = rd32(p + 4) >> 16;
    if (size < 8 || size % 4 || size > JWL_MSG_MAX || size > left)
        return fail(err, JWL_DISPLAY_ID, JWL_ERROR_INVALID_METHOD,
                    "a message of %u bytes for @%u (%u left in the batch)", size, id,
                    (unsigned)left);
    status_t st = dec_target(map, id, (uint16_t)(rd32(p + 4) & 0xffff), out, err);
    if (st != OK)
        return st;
    char where[80];
    snprintf(where, sizeof(where), "%s.%s", out->iface->name, out->msg->name);
    struct jwl_sig sig;
    if (jwl_sig_parse(out->msg->signature, &sig) != OK)
        return fail(err, id, JWL_ERROR_IMPLEMENTATION, "%s: a broken table", where);
    if (sig.since > out->version)
        return fail(err, id, JWL_ERROR_INVALID_METHOD, "%s@%u: since version %u, bound at %u",
                    where, id, sig.since, out->version);
    struct dec d = { .map = map, .err = err, .p = p + 8, .end = p + size, .in = in,
                     .hat = in->hat, .id = id, .where = where, .version = out->version };
    for (unsigned i = 0; i < sig.n; i++) {
        st = dec_arg(&d, &sig, i, out->msg->types, out->args);
        if (st != OK)
            return st;
    }
    if (d.p != d.end)
        return bad(&d, "bytes past the last argument");
    st = commit_new_ids(&d, out->dead_target);
    if (st != OK)
        return st;
    out->nargs = (uint16_t)sig.n;
    out->nhandles = d.hat - in->hat;
    in->at += size;
    in->hat = d.hat;
    return OK;
}

/* ---- encoding ----------------------------------------------------------------------- */

/* Bytes argument a takes on the wire (0 for a handle); -1 if it doesn't
 * fit its letter. */
static long arg_size(char type, bool nullable, const union jwl_arg *a)
{
    size_t len;
    switch (type) {
    case 's':
        if (!a->s)
            return nullable ? 4 : -1;
        len = strnlen(a->s, JWL_STRING_MAX) + 1;
        return len > JWL_STRING_MAX ? -1 : 4 + (long)padded((uint32_t)len);
    case 'a':
        if (a->a.size > JWL_STRING_MAX || (a->a.size && !a->a.data))
            return -1;
        return 4 + (long)padded(a->a.size);
    case 'o':
        return a->o || nullable ? 4 : -1;
    case 'n':
        return a->n ? 4 : -1;
    case 'h':
        return a->h != HANDLE_INVALID ? 0 : -1;
    }
    return 4;
}

static uint8_t *put_bytes(uint8_t *p, const void *src, uint32_t len)
{
    wr32(p, len);
    if (len)
        memcpy(p + 4, src, len);
    memset(p + 4 + len, 0, padded(len) - len);
    return p + 4 + padded(len);
}

static uint8_t *put_arg(uint8_t *p, char type, const union jwl_arg *a)
{
    switch (type) {
    case 's':
        return a->s ? put_bytes(p, a->s, (uint32_t)strlen(a->s) + 1) : put_bytes(p, NULL, 0);
    case 'a':
        return put_bytes(p, a->a.data, a->a.size);
    case 'h':
        return p;
    }
    wr32(p, a->u);   /* i, u, f, o and n share the word */
    return p + 4;
}

status_t jwl_encode(struct jwl_out *out, const struct jwl_message *m, uint32_t id,
                    uint16_t opcode, const union jwl_arg *args, unsigned nargs)
{
    struct jwl_sig sig;
    if (!m || jwl_sig_parse(m->signature, &sig) != OK || nargs != sig.n)
        return ERR_INVALID_ARGS;
    size_t size = 8;
    unsigned nh = 0;
    for (unsigned i = 0; i < sig.n; i++) {
        long n = arg_size(sig.type[i], sig.nullable[i], &args[i]);
        if (n < 0)
            return ERR_INVALID_ARGS;
        size += (size_t)n;
        nh += sig.type[i] == 'h';
    }
    if (size > JWL_MSG_MAX)
        return ERR_OUT_OF_RANGE;
    if (out->cap - out->len < size || out->hcap - out->nh < nh)
        return ERR_BUFFER_TOO_SMALL;
    uint8_t *p = out->buf + out->len;
    wr32(p, id);
    wr32(p + 4, (uint32_t)size << 16 | opcode);
    p += 8;
    for (unsigned i = 0; i < sig.n; i++) {
        p = put_arg(p, sig.type[i], &args[i]);
        if (sig.type[i] == 'h')
            out->h[out->nh++] = args[i].h;
    }
    out->len += size;
    return OK;
}
