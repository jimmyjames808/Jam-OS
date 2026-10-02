/* speed: the arguments and the lines (speed.h says what it does). */
#include <dns.h>
#include <ipv4.h>
#include <wants.h>
#include "speed.h"

/* What it is given (<wants.h>): the network with the listen permission
 * (speed -l), and the resolver for a host by name. */
JAM_WANTS("svc net listen\n"
          "svc dns\n");

#define ROLE_STOP (SR_USER + 2)   /* the shell's stop channel (sh_run_helper): Ctrl+C */

/* Ctrl+C: the shell writes a byte on the stop channel. Every wait of the
 * test is bounded, but a run, its report and a listener's wait for the
 * Mac are long, so this thread, which serves nothing, ends the program at
 * once (exit 130, as `fetch` does): its connection closes with it (a reset
 * when bytes were unread, else a FIN), and the shell needn't kill it. */
static void watch_stop(void *arg)
{
    signals_t seen;
    handle_t stop = (handle_t)(uintptr_t)arg;
    if (jam_object_wait_one(stop, SIG_READABLE, DEADLINE_NEVER, &seen) != OK)
        return;   /* the shell went: it can't ask any more */
    printf("speed: stopped\n");
    jam_process_exit(130);
}

static void stop_watched(void)
{
    static uint8_t stack[4096] __attribute__((aligned(16)));
    handle_t stop = startup_handle(ROLE_STOP), th;
    if (!stop)
        return;   /* run as a plain program: Ctrl+C kills it */
    if (thread_spawn("speed-stop", watch_stop, (void *)(uintptr_t)stop, stack, sizeof(stack),
                     &th) == OK)
        jam_handle_close(th);
}

void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)(v >> 32));
    put32(p + 4, (uint32_t)v);
}

uint64_t get64(const uint8_t *p)
{
    return (uint64_t)get32(p) << 32 | get32(p + 4);
}

void say_rate(const char *what, uint64_t n, uint64_t ns)
{
    uint64_t us = ns / 1000 ? ns / 1000 : 1;
    uint64_t mb10 = n * 10 / us;                 /* a byte a microsecond is 1 MB/s */
    uint64_t mbit10 = n * 80 / us;
    printf("speed: %s %lu.%lu MB in %lu.%02lu s: %lu.%lu MB/s, %lu.%lu Mbit/s\n", what,
           (unsigned long)(n / 1000000), (unsigned long)(n % 1000000 / 100000),
           (unsigned long)(ns / NS_PER_S), (unsigned long)(ns % NS_PER_S / (10 * NS_PER_MS)),
           (unsigned long)(mb10 / 10), (unsigned long)(mb10 % 10), (unsigned long)(mbit10 / 10),
           (unsigned long)(mbit10 % 10));
}

static bool number(const char *s, uint64_t lo, uint64_t hi, uint64_t *out)
{
    uint64_t v = 0;
    if (!*s)
        return false;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > (hi - (uint64_t)(*s - '0')) / 10)
            return false;
        v = v * 10 + (uint64_t)(*s - '0');
    }
    *out = v;
    return v >= lo;
}

static int usage(void)
{
    printf("usage: speed <host> [port] [-r | -u] [-t seconds] | speed -l [port]\n"
           "       (the Mac runs `python3 tools/speed.py server`; port %u)\n", SPEED_PORT);
    return 2;
}

/* The options after the host: [port] [-r] [-u] [-t s]. */
struct opts {
    uint64_t port, seconds;
    bool     receive, udp, listen;
    const char *host;
};

static bool parse(int argc, char **argv, struct opts *o)
{
    *o = (struct opts){ .port = SPEED_PORT, .seconds = SPEED_SECONDS };
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-t")) {
            if (++i == argc || !number(argv[i], 1, SPEED_MAX_S, &o->seconds))
                return false;
        } else if (!strcmp(a, "-r")) {
            o->receive = true;
        } else if (!strcmp(a, "-u")) {
            o->udp = true;
        } else if (!strcmp(a, "-l")) {
            o->listen = true;
        } else if (!o->host && !o->listen && a[0] != '-') {
            o->host = a;
        } else if (!number(a, 1, 65535, &o->port)) {
            return false;
        }
    }
    return o->listen ? !o->host && !o->receive && !o->udp
                     : o->host && !(o->receive && o->udp);
}

int main(int argc, char **argv)
{
    struct opts o;
    if (!parse(argc, argv, &o))
        return usage();
    stop_watched();
    if (o.listen)
        return tcp_listen((uint16_t)o.port);
    uint32_t addr;
    const char *end = NULL;
    if (!ipv4_parse(o.host, &addr, &end) || *end) {
        struct dns_answer a;
        status_t st = dns_lookup(o.host, now() + SPEED_WAIT, &a);
        if (st != OK) {
            printf("speed: %s: %s\n", o.host, st == ERR_NOT_FOUND ? "no such name"
                                                                  : status_str(st));
            return 1;
        }
        addr = a.addr[0];
    }
    if (o.udp)
        return udp_client(addr, (uint16_t)o.port, (uint32_t)o.seconds);
    return tcp_client(addr, (uint16_t)o.port, o.receive, (uint32_t)o.seconds);
}
