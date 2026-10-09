/* utest: the wire of init's one-shot channels with the splash and with
 * logd (M12 P6b): `splashch` (abi/idl/splashch.idl, <splash.h>) and
 * `crashres` (abi/idl/logctl.idl, <crashlog.h>), each read by the reader
 * its real user has, against messages built by hand, bytes and all.
 *
 *   splashch_wire   init's side (the generated server, as init/splash.c
 *                   runs it): `played` runs its handler once; the older
 *                   one-word forms (a bare 1 or 2), a call with a txid, the
 *                   other event's ordinal, another protocol's, bytes after
 *                   the header, a handle sent with it: none runs it; the
 *                   splash's side (the generated reader of `go`) takes
 *                   `go` the same way and nothing else
 *   crashres_wire   init's side (crashlog_read_result, what lastboot.c
 *                   calls): nothing queued is ERR_SHOULD_WAIT; the older
 *                   104-byte struct, a txid, a path past 95 bytes or not
 *                   UTF-8, a count past the bytes, bytes after, a handle:
 *                   each dropped and read past; the good result then
 *                   comes; a path with control characters is made
 *                   printable, a status above 0 an error; a channel that
 *                   closes with no result is ERR_PEER_CLOSED */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <crashlog.h>
#include <idl/logctl.h>
#include <idl/splashch.h>
#include <os.h>
#include "utest.h"

#define SPLASHCH_ID 83u
#define CRASHRES_ID 84u

/* ---- splashch --------------------------------------------------------------------- */

static void on_played(void *ctx)
{
    (*(unsigned *)ctx)++;
}

static void on_go(void *ctx)
{
    (*(unsigned *)ctx)++;
}

/* Write n bytes (zeros after the first words are mine) on c with a txid and ordinal. */
static status_t send_raw(handle_t c, uint32_t txid, uint32_t ordinal, uint32_t extra,
                         handle_t h)
{
    uint8_t m[24] = { 0 };
    memcpy(m, &txid, 4);
    memcpy(m + 4, &ordinal, 4);
    return jam_channel_write(c, m, 8 + extra, h ? &h : NULL, h ? 1 : 0);
}

/* Serve everything queued on the splash's channel; handled messages. */
static unsigned drain_played(handle_t srv, unsigned *played)
{
    static const struct splashch_ops ops = { .played = on_played };
    unsigned n = 0;
    while (splashch_serve_one(srv, &ops, played) == OK)
        n++;
    return n;
}

static unsigned drain_go(handle_t cli, unsigned *went)
{
    static const struct splashch_events events = { .go = on_go };
    unsigned n = 0;
    status_t st;
    while ((st = splashch_read_event(cli, &events, went)) != ERR_SHOULD_WAIT &&
           st != ERR_PEER_CLOSED)
        n++;
    return n;
}

bool t_splashch_wire(void)
{
    handle_t init, splash, extra, extra_peer;
    unsigned played = 0, went = 0;
    CHECK_ST(jam_channel_create(&init, &splash), OK);
    uint32_t played_ord = (SPLASHCH_ID << 16) | 1u, go_ord = (SPLASHCH_ID << 16) | 2u;

    /* init's side: the good event, once */
    CHECK_ST(splashch_played(splash), OK);
    CHECK_EQ(drain_played(init, &played), 1);
    CHECK_EQ(played, 1);
    /* the older forms: a bare word */
    uint32_t one = 1, two = 2;
    CHECK_ST(jam_channel_write(splash, &one, 4, NULL, 0), OK);
    CHECK_ST(jam_channel_write(splash, &two, 4, NULL, 0), OK);
    /* a call (a txid), the other event's ordinal, another protocol's,
     * bytes after the header, one byte short */
    CHECK_ST(send_raw(splash, 5, played_ord, 0, HANDLE_INVALID), OK);
    CHECK_ST(send_raw(splash, 0, go_ord, 0, HANDLE_INVALID), OK);
    CHECK_ST(send_raw(splash, 0, (18u << 16) | 1u, 0, HANDLE_INVALID), OK);
    CHECK_ST(send_raw(splash, 0, played_ord, 4, HANDLE_INVALID), OK);
    uint8_t cut[7] = { 0 };
    CHECK_ST(jam_channel_write(splash, cut, sizeof(cut), NULL, 0), OK);
    /* a handle with the event: closed, refused */
    CHECK_ST(jam_channel_create(&extra, &extra_peer), OK);
    CHECK_ST(send_raw(splash, 0, played_ord, 0, extra), OK);
    unsigned handled = drain_played(init, &played);
    CHECK(handled >= 8);   /* every one was read (and dropped or refused) */
    CHECK_EQ(played, 1);
    CHECK_ST(jam_object_wait_one(extra_peer, SIG_PEER_CLOSED, now() + NS_PER_S, NULL), OK);
    jam_handle_close(extra_peer);
    /* a good one after them all still runs */
    CHECK_ST(splashch_played(splash), OK);
    CHECK_EQ(drain_played(init, &played), 1);
    CHECK_EQ(played, 2);
    /* the splash's side: `go` once; the same garbage means nothing */
    CHECK_ST(jam_channel_write(init, &two, 4, NULL, 0), OK);
    CHECK_ST(send_raw(init, 0, played_ord, 0, HANDLE_INVALID), OK);
    CHECK_ST(send_raw(init, 7, go_ord, 0, HANDLE_INVALID), OK);
    CHECK_ST(send_raw(init, 0, go_ord, 4, HANDLE_INVALID), OK);
    CHECK_ST(send_raw(init, 0, (18u << 16) | 1u, 0, HANDLE_INVALID), OK);
    CHECK_ST(jam_channel_create(&extra, &extra_peer), OK);
    CHECK_ST(send_raw(init, 0, go_ord, 0, extra), OK);
    CHECK(drain_go(splash, &went) >= 6);
    CHECK_EQ(went, 0);
    CHECK_ST(jam_object_wait_one(extra_peer, SIG_PEER_CLOSED, now() + NS_PER_S, NULL), OK);
    jam_handle_close(extra_peer);
    CHECK_ST(splashch_go(init), OK);
    CHECK_EQ(drain_go(splash, &went), 1);
    CHECK_EQ(went, 1);
    /* closing the channel is what it means, too */
    jam_handle_close(init);
    CHECK_ST(splashch_read_event(splash, &(struct splashch_events){ 0 }, NULL), ERR_PEER_CLOSED);
    jam_handle_close(splash);
    return true;
}

/* ---- crashres --------------------------------------------------------------------- */

/* crashres.result by hand: txid, ordinal, status, a u16 count and `n` bytes of
 * path (count `count`), then `after` bytes more, and the handle. */
static status_t result_raw(handle_t c, int32_t status, const void *path, uint32_t n,
                           uint16_t count, uint32_t after, handle_t h)
{
    uint8_t m[160] = { 0 };
    uint32_t ordinal = (CRASHRES_ID << 16) | 1u;
    memcpy(m + 4, &ordinal, 4);
    memcpy(m + 8, &status, 4);
    memcpy(m + 12, &count, 2);
    memcpy(m + 14, path, n);
    return jam_channel_write(c, m, 14 + n + after, h ? &h : NULL, h ? 1 : 0);
}

bool t_crashres_wire(void)
{
    handle_t logd, init, extra, extra_peer;
    struct crashlog_result r;
    char path[100];
    CHECK_ST(jam_channel_create(&logd, &init), OK);
    CHECK_ST(crashlog_read_result(init, &r), ERR_SHOULD_WAIT);   /* nothing yet */
    CHECK(!r.have);
    /* the older struct: status, reserved, 96 bytes of path */
    uint8_t old[104] = { 0 };
    memcpy(old + 8, "/data/logs/boot-0001-crash.txt", 31);
    CHECK_ST(jam_channel_write(logd, old, sizeof(old), NULL, 0), OK);
    int32_t io = ERR_IO;
    memcpy(old, &io, 4);
    CHECK_ST(jam_channel_write(logd, old, sizeof(old), NULL, 0), OK);   /* a txid now */
    /* a txid on the event */
    uint8_t withtxid[16] = { 3, 0, 0, 0 };
    uint32_t ord = (CRASHRES_ID << 16) | 1u;
    memcpy(withtxid + 4, &ord, 4);
    CHECK_ST(jam_channel_write(logd, withtxid, sizeof(withtxid), NULL, 0), OK);
    /* a path past 95 bytes (96, with its count), a count past the bytes,
     * bytes after the path, a path that is no UTF-8 */
    memset(path, 'p', sizeof(path));
    CHECK_ST(result_raw(logd, 0, path, 96, 96, 0, HANDLE_INVALID), OK);
    CHECK_ST(result_raw(logd, 0, path, 10, 12, 0, HANDLE_INVALID), OK);
    CHECK_ST(result_raw(logd, 0, path, 10, 10, 3, HANDLE_INVALID), OK);
    CHECK_ST(result_raw(logd, 0, "a\xff\xc0\x80z", 5, 5, 0, HANDLE_INVALID), OK);
    CHECK_ST(jam_channel_create(&extra, &extra_peer), OK);
    CHECK_ST(result_raw(logd, 0, "x", 1, 1, 0, extra), OK);   /* a handle */
    /* all dropped, none is the result (a call reads at most 8 messages) */
    CHECK_ST(crashlog_read_result(init, &r), ERR_SHOULD_WAIT);
    CHECK_ST(crashlog_read_result(init, &r), ERR_SHOULD_WAIT);
    CHECK(!r.have);
    CHECK_ST(jam_object_wait_one(extra_peer, SIG_PEER_CLOSED, now() + NS_PER_S, NULL), OK);
    jam_handle_close(extra_peer);
    /* the good one, with a path of the most */
    memset(path, 'q', 95);
    path[95] = '\0';
    CHECK_ST(crashres_result(logd, ERR_IO, path, 95), OK);
    CHECK_ST(crashlog_read_result(init, &r), OK);
    CHECK(r.have);
    CHECK_ST(r.status, ERR_IO);
    CHECK_EQ(strlen(r.path), 95);
    CHECK(r.path[0] == 'q' && r.path[94] == 'q');
    jam_handle_close(logd);
    jam_handle_close(init);

    /* control characters in the path, and a status above 0 */
    CHECK_ST(jam_channel_create(&logd, &init), OK);
    CHECK_ST(result_raw(logd, 7, "a\x01\n\x7f" "b\xc3\xa9", 7, 7, 0, HANDLE_INVALID), OK);
    CHECK_ST(crashlog_read_result(init, &r), OK);
    CHECK_ST(r.status, ERR_INTERNAL);   /* 7 is no error code */
    CHECK(!strcmp(r.path, "a???b??"));
    jam_handle_close(logd);
    jam_handle_close(init);

    /* a result of OK and no path */
    CHECK_ST(jam_channel_create(&logd, &init), OK);
    CHECK_ST(crashres_result(logd, OK, "", 0), OK);
    CHECK_ST(crashlog_read_result(init, &r), OK);
    CHECK(r.status == OK && r.path[0] == '\0');
    jam_handle_close(logd);
    jam_handle_close(init);

    /* logd ended without a word */
    CHECK_ST(jam_channel_create(&logd, &init), OK);
    CHECK_ST(jam_channel_write(logd, old, sizeof(old), NULL, 0), OK);   /* only garbage */
    jam_handle_close(logd);
    CHECK_ST(crashlog_read_result(init, &r), ERR_PEER_CLOSED);
    CHECK(!r.have);
    jam_handle_close(init);
    return true;
}
