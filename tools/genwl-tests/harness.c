/* Test harness for tools/genwl.py, built and run by its selftest
 * (tools/genwl-tests/selftest.py) with the Mac's compiler, against a
 * stand-in <jwl.h> that holds the contract's two table structs and the
 * codec names the stubs call.
 *
 * It prints, one line each:
 *   I <interface> <version> <nrequests> <nevents>
 *   R|E <opcode> <name> <signature> <type per letter>   every table entry
 * then sends messages through the generated stubs into a jwl_send that keeps
 * what it was given, and hands that straight to the generated dispatchers:
 *   send <id> <opcode> <name> <signature> <letter:value ...>
 *   got <id> <opcode> <name> <signature> <letter:value ...>
 *   status <dispatcher's status>
 * selftest.py compares every line with what the XML and Wayland's wire
 * format say (it also encodes the sent arguments to bytes and checks them
 * against bytes written out by hand). */
#include <stdio.h>
#include <string.h>

#include <jwl/jwltest.h>
#include <jwl/wayland.h>
#include <jwl/xdg_shell.h>

/* Every interface of every protocol, NULL-terminated: all.c, written by
 * selftest.py. */
extern const struct jwl_interface *const genwl_all[];

struct jwl_conn {
    int unused;
};

static struct jwl_conn conn;
static int marker;      /* the object data every handler must get back */

/* What the last jwl_send was given. */
static struct {
    uint32_t id, opcode;
    const struct jwl_message *m;
    union jwl_arg args[20];
} last;

static size_t nletters(const char *sig)
{
    size_t n = 0;
    for (; *sig; sig++)
        if (*sig != '?' && (*sig < '0' || *sig > '9'))
            n++;
    return n;
}

status_t jwl_send(struct jwl_conn *c, uint32_t id, uint32_t opcode, const struct jwl_message *m,
                  const union jwl_arg *args)
{
    size_t n = nletters(m->signature);
    if (c != &conn || n > 20 || (n && !args))
        return ERR_INVALID_ARGS;
    last.id = id;
    last.opcode = opcode;
    last.m = m;
    if (n)
        memcpy(last.args, args, n * sizeof(*args));
    return OK;
}

static void print_arg(char l, const union jwl_arg *a)
{
    const unsigned char *p = a->a.data;
    switch (l) {
    case 'i':
    case 'f':
        printf(" %c:%d", l, a->i);
        break;
    case 'u':
    case 'o':
    case 'n':
        printf(" %c:%u", l, a->u);
        break;
    case 's':
        if (a->s)
            printf(" s:\"%s\"", a->s);
        else
            printf(" s:null");
        break;
    case 'a':
        printf(" a:");
        for (uint32_t k = 0; k < a->a.size; k++)
            printf("%02x", p[k]);
        break;
    case 'h':
        printf(" h:%u", a->h);
        break;
    default:
        printf(" ?%c", l);
    }
}

static void print_msg(const char *what, uint32_t id, uint32_t opcode,
                      const struct jwl_message *m, const union jwl_arg *a)
{
    printf("%s %u %u %s %s", what, id, opcode, m->name, *m->signature ? m->signature : "-");
    for (const char *s = m->signature; *s; s++)
        if (*s != '?' && (*s < '0' || *s > '9'))
            print_arg(*s, a++);
    putchar('\n');
}

static void dump_messages(char kind, const struct jwl_message *ms, unsigned n)
{
    for (unsigned k = 0; k < n; k++) {
        const struct jwl_message *m = &ms[k];
        printf("%c %u %s %s", kind, k, m->name, *m->signature ? m->signature : "-");
        for (size_t j = 0; j < nletters(m->signature); j++)
            printf(" %s", m->types[j] ? m->types[j]->name : "-");
        putchar('\n');
    }
}

static void dump(void)
{
    for (const struct jwl_interface *const *i = genwl_all; *i; i++) {
        printf("I %s %u %u %u\n", (*i)->name, (*i)->version, (*i)->nrequests, (*i)->nevents);
        dump_messages('R', (*i)->requests, (*i)->nrequests);
        dump_messages('E', (*i)->events, (*i)->nevents);
    }
}

/* A handler: print what it got, as the sender's line. */
static status_t got(void *data, uint32_t self, uint32_t opcode, const struct jwl_message *m,
                    const union jwl_arg *a)
{
    print_msg(data == &marker ? "got" : "got-wrong-data", self, opcode, m, a);
    return OK;
}

#define REQ(iface, op) (&jwl_##iface##_interface.requests[op])
#define EV(iface, op)  (&jwl_##iface##_interface.events[op])

/* Send with a stub, check it named the right table entry, dispatch what
 * jwl_send got. */
#define ROUND(send_call, table, dispatch, handlers)                                    \
    do {                                                                               \
        status_t st_ = (send_call);                                                    \
        if (st_ != OK)                                                                 \
            printf("send failed %d\n", st_);                                           \
        print_msg(last.m == &(table)[last.opcode] ? "send" : "send-wrong-msg", last.id, \
                  last.opcode, last.m, last.args);                                     \
        printf("status %d\n", dispatch(handlers, &marker, last.id, last.opcode, last.args)); \
    } while (0)

/* ---- The sample protocol (jwltest.xml) ---- */

static status_t t_make(void *data, uint32_t self, uint32_t id, uint32_t flags)
{
    const union jwl_arg a[] = { { .n = id }, { .u = flags } };
    return got(data, self, JWL_T_ROOT_REQ_MAKE, REQ(t_root, JWL_T_ROOT_REQ_MAKE), a);
}

static status_t t_bind(void *data, uint32_t self, uint32_t name, const char *interface,
                       uint32_t version, uint32_t id)
{
    const union jwl_arg a[] = { { .u = name }, { .s = interface }, { .u = version }, { .n = id } };
    return got(data, self, JWL_T_ROOT_REQ_BIND, REQ(t_root, JWL_T_ROOT_REQ_BIND), a);
}

static status_t t_every(void *data, uint32_t self, int32_t i, uint32_t u, int32_t f,
                        const char *s, const char *ns, uint32_t o, uint32_t no, uint32_t any,
                        const void *bytes, uint32_t bytes_size, handle_t fd)
{
    const union jwl_arg a[] = {
        { .i = i }, { .u = u }, { .f = f }, { .s = s }, { .s = ns }, { .o = o }, { .o = no },
        { .o = any }, { .a = { bytes, bytes_size } }, { .h = fd },
    };
    return got(data, self, JWL_T_ROOT_REQ_EVERY, REQ(t_root, JWL_T_ROOT_REQ_EVERY), a);
}

static status_t t_destroy(void *data, uint32_t self)
{
    return got(data, self, JWL_T_ROOT_REQ_DESTROY, REQ(t_root, JWL_T_ROOT_REQ_DESTROY), NULL);
}

static status_t t_poke(void *data, uint32_t self, int32_t self_, int32_t default_, uint32_t c_,
                       uint32_t jwl_a_)
{
    const union jwl_arg a[] = { { .i = self_ }, { .i = default_ }, { .u = c_ }, { .u = jwl_a_ } };
    return got(data, self, JWL_T_CHILD_REQ_POKE, REQ(t_child, JWL_T_CHILD_REQ_POKE), a);
}

static status_t t_ping(void *data, uint32_t self, uint32_t serial)
{
    const union jwl_arg a[] = { { .u = serial } };
    return got(data, self, JWL_T_ROOT_EV_PING, EV(t_root, JWL_T_ROOT_EV_PING), a);
}

static status_t t_gone(void *data, uint32_t self, uint32_t child)
{
    const union jwl_arg a[] = { { .o = child } };
    return got(data, self, JWL_T_ROOT_EV_GONE, EV(t_root, JWL_T_ROOT_EV_GONE), a);
}

static status_t t_done(void *data, uint32_t self, uint32_t data_)
{
    const union jwl_arg a[] = { { .u = data_ } };
    return got(data, self, JWL_T_CHILD_EV_DONE, EV(t_child, JWL_T_CHILD_EV_DONE), a);
}

static status_t t_hush(void *data, uint32_t self)
{
    return got(data, self, JWL_T_QUIET_REQ_HUSH, REQ(t_quiet, JWL_T_QUIET_REQ_HUSH), NULL);
}

static void sample(void)
{
    static const struct jwl_t_root_requests rr = {
        .make = t_make, .bind = t_bind, .every = t_every, .destroy = t_destroy,
    };
    static const struct jwl_t_root_events re = { .ping = t_ping, .gone = t_gone };
    static const struct jwl_t_child_requests cr = { .poke = t_poke };
    static const struct jwl_t_child_events ce = { .done = t_done };
    static const struct jwl_t_quiet_requests qr = { .hush = t_hush };
    static const struct jwl_t_root_requests none = { 0 };
    const struct jwl_message *rq = jwl_t_root_interface.requests;
    const struct jwl_message *ev = jwl_t_root_interface.events;

    ROUND(jwl_t_root_make(&conn, 1, 4, JWL_T_CHILD_FLAGS_TOP), rq,
          jwl_t_root_dispatch_request, &rr);
    ROUND(jwl_t_root_bind(&conn, 1, 5, "t_child", 1, 6), rq, jwl_t_root_dispatch_request, &rr);
    ROUND(jwl_t_root_every(&conn, 7, -5, JWL_T_ROOT_ERROR_WORSE, 256, "hi", NULL, 3, 0, 9,
                           "abc", 3, 42),
          rq, jwl_t_root_dispatch_request, &rr);
    ROUND(jwl_t_root_destroy(&conn, 7), rq, jwl_t_root_dispatch_request, &rr);
    ROUND(jwl_t_child_poke(&conn, 4, 1, 2, 3, 4), jwl_t_child_interface.requests,
          jwl_t_child_dispatch_request, &cr);
    ROUND(jwl_t_root_send_ping(&conn, 1, 77), ev, jwl_t_root_dispatch_event, &re);
    ROUND(jwl_t_root_send_gone(&conn, 1, 0), ev, jwl_t_root_dispatch_event, &re);
    ROUND(jwl_t_child_send_done(&conn, 4, 9), jwl_t_child_interface.events,
          jwl_t_child_dispatch_event, &ce);
    ROUND(jwl_t_quiet_hush(&conn, 8), jwl_t_quiet_interface.requests,
          jwl_t_quiet_dispatch_request, &qr);
    /* No handler, and an opcode t_root doesn't have. */
    printf("status %d\n", jwl_t_root_dispatch_request(&none, &marker, 7, 3, last.args));
    printf("status %d\n", jwl_t_root_dispatch_request(&rr, &marker, 7, 4, last.args));
}

/* ---- Upstream's protocols: the messages selftest.py has bytes for ---- */

static status_t w_get_registry(void *data, uint32_t self, uint32_t registry)
{
    const union jwl_arg a[] = { { .n = registry } };
    return got(data, self, 1, REQ(wl_display, 1), a);
}

static status_t w_bind(void *data, uint32_t self, uint32_t name, const char *interface,
                       uint32_t version, uint32_t id)
{
    const union jwl_arg a[] = { { .u = name }, { .s = interface }, { .u = version }, { .n = id } };
    return got(data, self, 0, REQ(wl_registry, 0), a);
}

static status_t w_attach(void *data, uint32_t self, uint32_t buffer, int32_t x, int32_t y)
{
    const union jwl_arg a[] = { { .o = buffer }, { .i = x }, { .i = y } };
    return got(data, self, 1, REQ(wl_surface, 1), a);
}

static status_t w_create_pool(void *data, uint32_t self, uint32_t id, handle_t fd, int32_t size)
{
    const union jwl_arg a[] = { { .n = id }, { .h = fd }, { .i = size } };
    return got(data, self, 0, REQ(wl_shm, 0), a);
}

static status_t w_configure(void *data, uint32_t self, int32_t width, int32_t height,
                            const void *states, uint32_t states_size)
{
    const union jwl_arg a[] = { { .i = width }, { .i = height }, { .a = { states, states_size } } };
    return got(data, self, 0, EV(xdg_toplevel, 0), a);
}

static status_t w_motion(void *data, uint32_t self, uint32_t time, int32_t x, int32_t y)
{
    const union jwl_arg a[] = { { .u = time }, { .f = x }, { .f = y } };
    return got(data, self, 2, EV(wl_pointer, 2), a);
}

static status_t w_error(void *data, uint32_t self, uint32_t object_id, uint32_t code,
                        const char *message)
{
    const union jwl_arg a[] = { { .o = object_id }, { .u = code }, { .s = message } };
    return got(data, self, 0, EV(wl_display, 0), a);
}

static status_t w_accept(void *data, uint32_t self, uint32_t serial, const char *mime_type)
{
    const union jwl_arg a[] = { { .u = serial }, { .s = mime_type } };
    return got(data, self, 0, REQ(wl_data_offer, 0), a);
}

static status_t w_keymap(void *data, uint32_t self, uint32_t format, handle_t fd, uint32_t size)
{
    const union jwl_arg a[] = { { .u = format }, { .h = fd }, { .u = size } };
    return got(data, self, 0, EV(wl_keyboard, 0), a);
}

static status_t w_set_title(void *data, uint32_t self, const char *title)
{
    const union jwl_arg a[] = { { .s = title } };
    return got(data, self, 2, REQ(xdg_toplevel, 2), a);
}

static status_t w_name(void *data, uint32_t self, const char *name)
{
    const union jwl_arg a[] = { { .s = name } };
    return got(data, self, 1, EV(wl_seat, 1), a);
}

static status_t w_wm_capabilities(void *data, uint32_t self, const void *caps, uint32_t caps_size)
{
    const union jwl_arg a[] = { { .a = { caps, caps_size } } };
    return got(data, self, 3, EV(xdg_toplevel, 3), a);
}

static void upstream(void)
{
    static const struct jwl_wl_display_requests dr = { .get_registry = w_get_registry };
    static const struct jwl_wl_display_events de = { .error = w_error };
    static const struct jwl_wl_registry_requests gr = { .bind = w_bind };
    static const struct jwl_wl_surface_requests sr = { .attach = w_attach };
    static const struct jwl_wl_shm_requests mr = { .create_pool = w_create_pool };
    static const struct jwl_xdg_toplevel_events te = {
        .configure = w_configure, .wm_capabilities = w_wm_capabilities,
    };
    static const struct jwl_xdg_toplevel_requests tr = { .set_title = w_set_title };
    static const struct jwl_wl_pointer_events pe = { .motion = w_motion };
    static const struct jwl_wl_data_offer_requests dor = { .accept = w_accept };
    static const struct jwl_wl_keyboard_events ke = { .keymap = w_keymap };
    static const struct jwl_wl_seat_events se = { .name = w_name };
    static const uint8_t states[8] = { 1, 0, 0, 0, 4, 0, 0, 0 };

    ROUND(jwl_wl_display_get_registry(&conn, 1, 2), jwl_wl_display_interface.requests,
          jwl_wl_display_dispatch_request, &dr);
    ROUND(jwl_wl_registry_bind(&conn, 2, 1, "wl_compositor", 4, 3),
          jwl_wl_registry_interface.requests, jwl_wl_registry_dispatch_request, &gr);
    ROUND(jwl_wl_surface_attach(&conn, 5, 0, -1, 2), jwl_wl_surface_interface.requests,
          jwl_wl_surface_dispatch_request, &sr);
    ROUND(jwl_wl_shm_create_pool(&conn, 4, 6, 77, 4096), jwl_wl_shm_interface.requests,
          jwl_wl_shm_dispatch_request, &mr);
    ROUND(jwl_xdg_toplevel_send_configure(&conn, 7, 800, 600, states, sizeof(states)),
          jwl_xdg_toplevel_interface.events, jwl_xdg_toplevel_dispatch_event, &te);
    ROUND(jwl_wl_pointer_send_motion(&conn, 8, 1000, 0xa80, -256),
          jwl_wl_pointer_interface.events, jwl_wl_pointer_dispatch_event, &pe);
    ROUND(jwl_wl_display_send_error(&conn, 1, 5, JWL_WL_DISPLAY_ERROR_NO_MEMORY, "bad"),
          jwl_wl_display_interface.events, jwl_wl_display_dispatch_event, &de);
    ROUND(jwl_wl_data_offer_accept(&conn, 10, 9, NULL), jwl_wl_data_offer_interface.requests,
          jwl_wl_data_offer_dispatch_request, &dor);
    ROUND(jwl_wl_keyboard_send_keymap(&conn, 11, JWL_WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, 78, 4096),
          jwl_wl_keyboard_interface.events, jwl_wl_keyboard_dispatch_event, &ke);
    ROUND(jwl_xdg_toplevel_set_title(&conn, 7, "Jam"), jwl_xdg_toplevel_interface.requests,
          jwl_xdg_toplevel_dispatch_request, &tr);
    ROUND(jwl_wl_seat_send_name(&conn, 12, "seat0"), jwl_wl_seat_interface.events,
          jwl_wl_seat_dispatch_event, &se);
    ROUND(jwl_xdg_toplevel_send_wm_capabilities(&conn, 7, NULL, 0),
          jwl_xdg_toplevel_interface.events, jwl_xdg_toplevel_dispatch_event, &te);
}

int main(void)
{
    dump();
    sample();
    upstream();
    return 0;
}
