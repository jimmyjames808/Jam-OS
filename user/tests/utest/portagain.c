/* utest: port_again (<os.h>), a loop's next turn on a channel it stopped
 * reading with messages still queued. A PERSISTENT binding fires when the
 * channel turns readable, not while it stays so: a reader that keeps to a
 * budget a turn gets no packet for what it left, nor for anything written
 * after, unless it asks for its next turn. port_again asks with a user
 * packet; with the port full of them, by binding the channel again, which
 * fires at once since it is readable. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include "utest.h"

#define KEY    0x51u
#define OTHER  0x52u                              /* the user packets that fill the port */
#define MASK   (SIG_READABLE | SIG_PEER_CLOSED)
#define BUDGET 8u                                 /* messages read a turn */

/* One turn on b: BUDGET messages at most; how many were read. */
static unsigned turn(handle_t b)
{
    unsigned n = 0;
    for (; n < BUDGET; n++) {
        uint32_t word = 0, got = 0, nh = 0;
        status_t st = jam_channel_read(&(struct channel_read_args){
            .h = b, .rd_cap = sizeof(word), .rd_bytes = (uint64_t)(uintptr_t)&word,
            .rd_actual = (uint64_t)(uintptr_t)&got, .rd_hactual = (uint64_t)(uintptr_t)&nh,
        });
        if (st != OK)
            break;
    }
    return n;
}

static bool write_some(handle_t a, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        CHECK_ST(jam_channel_write(a, &i, sizeof(i), NULL, 0), OK);
    return true;
}

/* The next packet: OK and its key and type, or the wait's status. */
static status_t next(handle_t port, uint64_t *key, uint32_t *type)
{
    struct port_packet p;
    status_t st = jam_port_wait(port, 0, &p);
    *key = p.key;
    *type = p.type;
    return st;
}

bool t_port_again(void)
{
    handle_t a, b, port;
    uint64_t key = 0;
    uint32_t type = 0;
    CHECK_ST(jam_channel_create(&a, &b), OK);
    CHECK_ST(jam_port_create(&port), OK);
    CHECK_ST(jam_port_bind(port, b, KEY, MASK, PORT_BIND_PERSISTENT), OK);
    if (!write_some(a, 2 * BUDGET + 4))
        return false;
    CHECK_ST(next(port, &key, &type), OK);
    CHECK(key == KEY && type == PORT_PACKET_SIGNAL);
    CHECK_EQ(turn(b), BUDGET);
    /* What is left, and what comes now, raise no edge: the reader is deaf. */
    CHECK_ST(next(port, &key, &type), ERR_TIMED_OUT);
    if (!write_some(a, 1))
        return false;
    CHECK_ST(next(port, &key, &type), ERR_TIMED_OUT);

    /* Asked for: a user packet with the binding's key. */
    CHECK_ST(port_again(port, b, KEY, MASK), OK);
    CHECK_ST(next(port, &key, &type), OK);
    CHECK(key == KEY && type == PORT_PACKET_USER);
    CHECK_EQ(turn(b), BUDGET);

    /* The port takes no more user packets (its bound, or our job's
     * memory): the binding is made again, and fires at once. */
    unsigned queued = 0;
    struct port_packet o = { .key = OTHER, .type = PORT_PACKET_USER };
    while (queued <= PORT_MAX_USER_PACKETS && jam_port_queue(port, &o) == OK)
        queued++;
    CHECK(queued <= PORT_MAX_USER_PACKETS);
    CHECK_ST(port_again(port, b, KEY, MASK), OK);
    for (unsigned i = 0; i < queued; i++) {
        CHECK_ST(next(port, &key, &type), OK);
        CHECK_EQ(key, OTHER);
    }
    CHECK_ST(next(port, &key, &type), OK);
    CHECK(key == KEY && type == PORT_PACKET_SIGNAL);
    CHECK_EQ(turn(b), 5);   /* the rest: 2 * BUDGET + 4 + 1 - 2 * BUDGET */
    CHECK_ST(next(port, &key, &type), ERR_TIMED_OUT);

    /* Read to empty, the new binding fires on the next edge as the old one did. */
    if (!write_some(a, 1))
        return false;
    CHECK_ST(next(port, &key, &type), OK);
    CHECK(key == KEY && type == PORT_PACKET_SIGNAL);
    CHECK_EQ(turn(b), 1);
    CHECK_ST(jam_handle_close(port), OK);
    CHECK_ST(jam_handle_close(a), OK);
    CHECK_ST(jam_handle_close(b), OK);
    return true;
}
