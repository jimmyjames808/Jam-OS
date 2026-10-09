/* utest: the console in window mode (bin/console, user/services/console)
 * against a compositor that misbehaves (M12.1, review 2b's hostile
 * tests). The console runs as init starts a terminal: its ADMIN channel
 * (CONSOLE_SR_CHANNEL) is ours, and so is the /svc/wayland it gets
 * (CONSOLE_SR_WAYLAND): every svc.connect it makes is answered here.
 *
 * t_con_hostile_compositor:
 *   1. each of its first connections gets garbage at once (bytes that are
 *      no batch, a wrong magic, a message to nothing, a run of 65 empty
 *      batches, a batch over 16 KiB, a header naming more handles than it
 *      brings, 29 handles, a delete_id of nothing); each time the console
 *      drops the connection and connects again, and answers console.size
 *      throughout;
 *   2. then a working compositor (the fake, jwlcfake.h) that sends hostile
 *      values: sizes of 4000x3000, 1x1, 0x0 and negative, the pointer
 *      entering and moving past every edge and a drag with it, the wheel
 *      at INT32_MIN, key codes past every table, every modifier bit, the
 *      copy and paste keys with no clipboard, a repeat rate of INT32_MAX
 *      with a key held, a keymap of junk, an output of negative size, a
 *      burst of pings and frame callbacks that never come; the console
 *      keeps answering and its grid stays inside 480x180;
 *   3. the close box: the console ends with exit code 0. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/console.h>
#include <idl/svc.h>
#include <jwl.h>
#include <jwl_client.h>
#include <keymap.h>
#include <os.h>
#include "jwlcfake.h"
#include "utest.h"

#define CON_WAIT     (5 * NS_PER_S)
#define KEY_LEFTMETA 125u
#define KEY_C        46u
#define KEY_V        47u
#define KEY_A        30u

struct con {
    handle_t job, proc;
    handle_t admin;   /* console, ADMIN */
    handle_t svc;     /* the server end of its /svc/wayland */
};

static status_t make_pair(void *ctx, handle_t *out)
{
    handle_t *server = ctx;
    return jam_channel_create(server, out);
}

/* The console's next svc.connect, answered: our end of its connection. */
static bool next_connect(struct con *k, handle_t *server)
{
    *server = HANDLE_INVALID;
    signals_t seen = 0;
    status_t st = jam_object_wait_one(k->svc, SIG_READABLE, now() + CON_WAIT, &seen);
    if (st != OK)
        FAIL("no svc.connect from the console: %s", status_str(st));
    static const struct svc_ops ops = { .connect = make_pair };
    CHECK_ST(svc_serve_one(k->svc, &ops, server), OK);
    CHECK(*server != HANDLE_INVALID);
    return true;
}

/* The console answers console.size, with a grid inside its bounds. */
static bool answers(struct con *k, uint16_t *cols, uint16_t *rows)
{
    uint16_t c = 0, r = 0;
    status_t st = console_size_within(k->admin, CON_WAIT, &c, &r);
    if (st != OK)
        FAIL("console.size: %s", status_str(st));
    CHECK(c >= 1 && c <= 480 && r >= 1 && r <= 180);
    if (cols)
        *cols = c;
    if (rows)
        *rows = r;
    return true;
}

/* ---- 1. garbage at once ---------------------------------------------------------------- */

static handle_t an_event(void)
{
    handle_t h = HANDLE_INVALID;
    (void)jam_event_create(&h);
    return h;
}

static uint32_t buf[5200];

/* Garbage case i into ch (the console's connection): false when there are
 * no more cases; *ok, whether the kernel took the write. */
static bool garbage(unsigned i, handle_t ch, bool *ok)
{
    handle_t hs[29];
    uint32_t nh = 0, n = 0;   /* words */
    *ok = true;
    switch (i) {
    case 0:
        *ok = jam_channel_write(ch, "\x4a\x57\x4c\x32\x00\x00\x00", 7, NULL, 0) == OK;
        return true;
    case 1:
        buf[n++] = 0x314c574a;   /* JWL1 */
        buf[n++] = 0, buf[n++] = 0, buf[n++] = 0;
        break;
    case 2:   /* an event on object 77, which was never made */
        buf[n++] = JWL_MAGIC, buf[n++] = 0, buf[n++] = 0, buf[n++] = 0;
        buf[n++] = 77, buf[n++] = 12u << 16 | 0, buf[n++] = 5;
        break;
    case 3: { /* a run of 65 empty batches (M12.1 C1) */
        uint32_t e[4] = { JWL_MAGIC, 0, 0, 0 };
        for (unsigned k = 0; k <= JWL_EMPTY_RUN && *ok; k++)
            *ok = jam_channel_write(ch, e, sizeof(e), NULL, 0) == OK;
        return true;
    }
    case 4:   /* over 16 KiB */
        buf[n++] = JWL_MAGIC, buf[n++] = 0, buf[n++] = 0, buf[n++] = 0;
        n = 5000;
        for (unsigned k = 4; k < n; k++)
            buf[k] = 0;
        break;
    case 5:   /* names two handles, brings one */
        buf[n++] = JWL_MAGIC, buf[n++] = 2, buf[n++] = 0, buf[n++] = 0;
        hs[nh++] = an_event();
        break;
    case 6:   /* 29 handles */
        buf[n++] = JWL_MAGIC, buf[n++] = 29, buf[n++] = 0, buf[n++] = 0;
        while (nh < 29)
            hs[nh++] = an_event();
        break;
    case 7:   /* delete_id of an id never made */
        buf[n++] = JWL_MAGIC, buf[n++] = 0, buf[n++] = 0, buf[n++] = 0;
        buf[n++] = JWL_DISPLAY_ID, buf[n++] = 12u << 16 | 1, buf[n++] = 999;
        break;
    default:
        return false;
    }
    /* the kernel may refuse 29 handles or 20,000 bytes itself (the
     * handles go either way): then we close the connection instead */
    *ok = jam_channel_write(ch, buf, n * 4, hs, nh) == OK;
    return true;
}

static bool garbage_rounds(struct con *k)
{
    bool wrote;
    for (unsigned i = 0;; i++) {
        handle_t server;
        CHECK(next_connect(k, &server));
        if (!garbage(i, server, &wrote)) {
            jam_handle_close(server);   /* the last connection: closed, it connects again */
            break;
        }
        if (wrote) {
            signals_t seen = 0;
            status_t st = jam_object_wait_one(server, SIG_PEER_CLOSED, now() + CON_WAIT, &seen);
            if (st != OK)
                FAIL("garbage case %u: the console kept the connection (%s)", i,
                     status_str(st));
        }
        jam_handle_close(server);
        CHECK(answers(k, NULL, NULL));
    }
    return true;
}

/* ---- 2. a compositor that works, with hostile values ------------------------------------ */

/* Serve for ms milliseconds. */
static void serve_for(struct fake *f, unsigned ms)
{
    uint64_t until = now() + (uint64_t)ms * NS_PER_MS;
    while (now() < until) {
        fake_serve(f);
        (void)jam_sleep_until(now() + NS_PER_MS);
    }
}

/* A live object of iface on the fake's connection (the first), 0 if none. */
static uint32_t object_of(struct fake *f, const struct jwl_interface *iface)
{
    const struct jwl_range *r = &f->conn->map.r[0];
    for (uint32_t i = 0; i < r->n; i++)
        if (r->v[i].state == JWL_LIVE && jwl_interface_same(r->v[i].iface, iface))
            return i + 1;
    return 0;
}

static bool sizes(struct con *k, struct fake *f, struct fake_surface *s)
{
    const uint32_t max = JWL_STATE(JWL_XDG_TOPLEVEL_STATE_MAXIMIZED);
    uint16_t c, r;
    CHECK_ST(fake_configure(f, s, 4000, 3000, max), OK);
    serve_for(f, 300);
    CHECK(answers(k, &c, &r));
    CHECK(c > 104 && r > 27);   /* bigger than its first grid */
    CHECK_ST(fake_configure(f, s, 1, 1, max), OK);
    serve_for(f, 100);
    CHECK(answers(k, &c, &r));
    CHECK(c == 1 && r == 1);
    static const int32_t odd[][2] = { { 0, 0 }, { -5, -7 }, { INT32_MIN, INT32_MAX } };
    for (unsigned i = 0; i < 3; i++) {
        CHECK_ST(fake_configure(f, s, odd[i][0], odd[i][1], max), OK);
        serve_for(f, 50);
        CHECK(answers(k, NULL, NULL));
    }
    CHECK_ST(fake_configure(f, s, 800, 600, max), OK);
    serve_for(f, 100);
    return answers(k, NULL, NULL);
}

static bool pointer(struct con *k, struct fake *f, struct fake_surface *s)
{
    struct jwl_conn *c = f->conn;
    CHECK_ST(jwl_wl_pointer_send_enter(c, f->pointer, f->serial++, s->id, INT32_MIN, INT32_MAX),
             OK);
    CHECK_ST(jwl_wl_pointer_send_button(c, f->pointer, f->serial++, 1, 0x110,
                                        JWL_WL_POINTER_BUTTON_STATE_PRESSED), OK);
    static const int32_t at[][2] = { { INT32_MAX, INT32_MIN }, { -256, -256 },
                                     { 40 * 256, 90 * 256 }, { INT32_MIN, INT32_MIN } };
    for (unsigned i = 0; i < 4; i++)
        CHECK_ST(jwl_wl_pointer_send_motion(c, f->pointer, 2 + i, at[i][0], at[i][1]), OK);
    CHECK_ST(jwl_wl_pointer_send_button(c, f->pointer, f->serial++, 9, 0x110,
                                        JWL_WL_POINTER_BUTTON_STATE_RELEASED), OK);
    for (int i = 0; i < 3; i++) {
        CHECK_ST(jwl_wl_pointer_send_axis_discrete(c, f->pointer, 0, i ? INT32_MAX : INT32_MIN),
                 OK);
        CHECK_ST(jwl_wl_pointer_send_axis(c, f->pointer, 10, 0, INT32_MIN), OK);
        CHECK_ST(jwl_wl_pointer_send_frame(c, f->pointer), OK);
    }
    CHECK_ST(jwl_wl_pointer_send_button(c, f->pointer, f->serial++, 11, 0xffffffffu, 7), OK);
    CHECK_ST(jwl_wl_pointer_send_leave(c, f->pointer, f->serial++, s->id), OK);
    serve_for(f, 100);
    return answers(k, NULL, NULL);
}

static bool keys(struct con *k, struct fake *f, struct fake_surface *s)
{
    struct jwl_conn *c = f->conn;
    CHECK_ST(fake_kb_enter(f, s), OK);
    CHECK_ST(fake_key(f, 0xffffffffu, true), OK);
    CHECK_ST(fake_key(f, 0xffffffffu, false), OK);
    CHECK_ST(fake_mods(f, 0xffffffffu, 0xffffffffu), OK);
    CHECK_ST(fake_key(f, KEY_A, true), OK);
    CHECK_ST(fake_key(f, KEY_A, false), OK);
    CHECK_ST(fake_mods(f, KEYMAP_MOD_SUPER, 0), OK);   /* copy and paste, no clipboard */
    for (unsigned i = 0; i < 2; i++) {
        CHECK_ST(fake_key(f, i ? KEY_V : KEY_C, true), OK);
        CHECK_ST(fake_key(f, i ? KEY_V : KEY_C, false), OK);
    }
    CHECK_ST(fake_mods(f, 0, 0), OK);
    CHECK_ST(jwl_wl_keyboard_send_repeat_info(c, f->keyboard, INT32_MAX, 0), OK);
    CHECK_ST(fake_key(f, KEY_A, true), OK);   /* held: the console repeats it */
    serve_for(f, 200);
    CHECK(answers(k, NULL, NULL));
    CHECK_ST(fake_key(f, KEY_A, false), OK);
    handle_t junk;
    CHECK_ST(jam_vmo_create(PAGE_SIZE, 0, &junk), OK);
    CHECK_ST(jam_vmo_write(junk, 0, "\xff\xfe junk", 7), OK);
    CHECK_ST(jwl_wl_keyboard_send_keymap(c, f->keyboard, JWL_WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                                         junk, 0xffffffffu), OK);
    CHECK_ST(jwl_wl_keyboard_send_leave(c, f->keyboard, f->serial++, s->id), OK);
    serve_for(f, 100);
    return answers(k, NULL, NULL);
}

static bool the_rest(struct con *k, struct fake *f, struct fake_surface *s)
{
    struct jwl_conn *c = f->conn;
    uint32_t out = object_of(f, &jwl_wl_output_interface);
    CHECK(out);
    CHECK_ST(jwl_wl_output_send_mode(c, out, JWL_WL_OUTPUT_MODE_CURRENT, -1, INT32_MIN, 0), OK);
    CHECK_ST(jwl_wl_output_send_scale(c, out, 0), OK);
    for (unsigned i = 0; i < 500; i++)
        CHECK_ST(fake_ping(f), OK);
    f->hold_frames = true;   /* frame callbacks never come */
    CHECK_ST(fake_configure(f, s, 640, 480, 0), OK);
    serve_for(f, 300);
    CHECK(answers(k, NULL, NULL));
    CHECK(f->pongs >= 500);
    f->hold_frames = false;
    return true;
}

static bool hostile_values(struct con *k)
{
    handle_t server;
    CHECK(next_connect(k, &server));
    static struct fake f;
    fake_init(&f);
    __atomic_store_n(&f.pending, server, __ATOMIC_RELEASE);
    struct fake_surface *s = NULL;
    uint64_t until = now() + CON_WAIT;
    while (now() < until && !((s = fake_surface(&f, 0)) && s->current && s->commits >= 2))
        serve_for(&f, 2);
    if (!s || !s->current)
        FAIL("the console's window never showed a buffer");
    CHECK(answers(k, NULL, NULL));
    bool ok = sizes(k, &f, s) && pointer(k, &f, s) && keys(k, &f, s) && the_rest(k, &f, s);
    if (ok) {   /* 3. the close box: the console ends, with 0 */
        CHECK_ST(fake_close(&f, s), OK);
        serve_for(&f, 10);
        struct process_info info;
        CHECK_ST(spawn_wait(k->proc, CON_WAIT, &info), OK);
        CHECK_EQ(info.killed, 0);
        CHECK_EQ(info.exit_code, 0);
    }
    fake_free(&f);
    return ok;
}

bool t_con_hostile_compositor(void)
{
    struct con k = { 0 };
    handle_t admin_theirs, svc_theirs;
    CHECK_ST(new_job(&k.job), OK);
    CHECK_ST(jam_channel_create(&k.admin, &admin_theirs), OK);
    CHECK_ST(jam_channel_create(&k.svc, &svc_theirs), OK);
    const char *argv[] = { "console", "term=3" };
    struct spawn_handle x[] = { { CONSOLE_SR_CHANNEL, admin_theirs },
                                { CONSOLE_SR_WAYLAND, svc_theirs } };
    struct spawn_args a = { .path = "bin/console", .name = "console-hostile", .argc = 2,
                            .argv = argv, .job = k.job, .extra = x, .nextra = 2 };
    CHECK_ST(spawn(&a, &k.proc), OK);
    bool ok = answers(&k, NULL, NULL) && garbage_rounds(&k) && hostile_values(&k);
    if (!ok)
        (void)jam_job_kill(k.job);
    jam_handle_close(k.proc);
    jam_handle_close(k.admin);
    jam_handle_close(k.svc);
    jam_handle_close(k.job);
    return ok;
}
