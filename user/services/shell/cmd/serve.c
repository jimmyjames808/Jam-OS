/* serve: one file over HTTP to any computer on the network, in the
 * background (abi/idl/serve.idl; the server is bin/serve, a service init
 * runs, user/services/serve). The shell opens the file read-only and hands
 * it over; the server serves exactly that file, to every path asked, and
 * never opens anything itself. The shell is free again at once.
 *   serve <file> [port]   serve it on port (8080 unless given; 1024 and up)
 *   serve                 what is served: file, port, clients, requests, bytes
 *   serve stop [port]     stop it (every file without a port)
 * Exit: 0, 1 (it can't), 2 (usage). */
#include <idl/serve.h>
#include <ipv4.h>
#include <net.h>
#include <serve.h>
#include "sh.h"

#define SOON (5 * NS_PER_S)

/* This machine's address, for the URL lines ("<address>" if none yet). */
static const char *address(char buf[IPV4_TEXT_MAX])
{
    struct net_info i;
    handle_t net = svc_get(SVC_NET);
    if (!net || net_info(net, &i) != OK || !i.address)
        return "<address>";
    return ipv4_format(i.address, buf);
}

static const char *why(status_t st)
{
    switch (st) {
    case ERR_ACCESS_DENIED:  return "bin/serve has no listen permission (its list: svc net listen)";
    case ERR_ALREADY_BOUND:  return "another program has that port";
    case ERR_ALREADY_EXISTS: return "a file is served on that port already (serve stop <port>)";
    case ERR_NO_RESOURCES:   return "4 files are served already (serve stop <port>)";
    case ERR_BAD_STATE:      return "no network address yet (see `net`)";
    case ERR_TIMED_OUT:      return "the server didn't answer";
    default:                 return sh_why(st);
    }
}

static int list(handle_t ch)
{
    char a[IPV4_TEXT_MAX], size[24];
    unsigned i = 0;
    for (;; i++) {   /* at most SERVE_SHARES answers before ERR_NOT_FOUND */
        uint8_t name[SERVE_NAME_MAX];
        uint16_t port = 0;
        uint64_t bytes = 0, requests = 0, sz = 0;
        uint32_t n = 0;
        status_t st = serve_info_until(ch, now() + SOON, i, &port, name, &sz, &n, &requests,
                                       &bytes);
        if (st == ERR_NOT_FOUND)
            break;
        if (st != OK) {
            sh_tty("serve: the server doesn't answer (%s)\n", status_str(st));
            return 1;
        }
        name[SERVE_NAME_MAX - 1] = 0;
        sh_say("serve: port %u: %s (%s): %u client%s now, %lu request%s, %lu bytes sent\n"
               "  http://%s:%u/\n", port, (char *)name, sh_human(sz, size, sizeof(size)), n,
               n == 1 ? "" : "s", (unsigned long)requests, requests == 1 ? "" : "s",
               (unsigned long)bytes, address(a), port);
    }
    if (!i)
        sh_say("serve: nothing is served (serve <file> [port])\n");
    return 0;
}

static int stop(handle_t ch, uint16_t port)
{
    uint32_t n = 0;
    status_t st = serve_stop_until(ch, now() + SOON, port, &n);
    if (st != OK) {
        sh_tty("serve: the server doesn't answer (%s)\n", status_str(st));
        return 1;
    }
    if (!n)
        sh_say("serve: nothing was served%s\n", port ? " on that port" : "");
    else
        sh_say("serve: stopped %u file%s\n", n, n == 1 ? "" : "s");
    return 0;
}

/* The file, opened read-only, or false (said why). */
static bool open_file(const char *arg, char abs[SH_PATH_MAX], struct jfile *f)
{
    bool dir = false;
    uint64_t size = 0;
    status_t st = sh_resolve(arg, abs, SH_PATH_MAX) ? sh_stat(abs, &dir, &size) : ERR_NOT_FOUND;
    if (st == OK && dir) {
        sh_tty("serve: %s: a folder (serve serves one file)\n", arg);
        return false;
    }
    if (st == OK)
        st = file_open(abs, FS_READ, f);
    if (st != OK)
        sh_tty("serve: %s: %s\n", arg, st == ERR_NOT_FOUND ? "no such file" : sh_why(st));
    return st == OK;
}

/* Hand f over on give and read the server's answer. */
static status_t give_file(handle_t give, struct jfile *f, struct serve_answer *ans)
{
    struct serve_give g = { .magic = SERVE_GIVE_MAGIC, .size = f->size };
    handle_t hs[2];
    file_give(f, &hs[0], &hs[1]);
    status_t st = jam_channel_write(give, &g, sizeof(g), hs, 2);
    if (st != OK) {
        jam_handle_close(hs[0]);
        jam_handle_close(hs[1]);
        return st;
    }
    signals_t seen;
    uint32_t n = 0, nh = 0;
    st = jam_object_wait_one(give, SIG_READABLE | SIG_PEER_CLOSED, now() + 2 * SOON, &seen);
    struct channel_read_args a = {
        .h = give, .bytes_cap = sizeof(*ans), .bytes = (uint64_t)(uintptr_t)ans,
        .actual_bytes = (uint64_t)(uintptr_t)&n, .actual_handles = (uint64_t)(uintptr_t)&nh,
    };
    if (st == OK)
        st = jam_channel_read(&a);
    if (st == OK && n != sizeof(*ans))
        st = ERR_INTERNAL;
    return st;
}

static int start(handle_t ch, const char *arg, uint16_t port)
{
    char abs[SH_PATH_MAX], a[IPV4_TEXT_MAX], size[24];
    struct jfile f;
    if (!open_file(arg, abs, &f))
        return 1;
    uint8_t name[SERVE_NAME_MAX] = { 0 };
    size_t n = strlen(abs), from = n >= SERVE_NAME_MAX ? n - (SERVE_NAME_MAX - 1) : 0;
    memcpy(name, abs + from, n - from);   /* a long path keeps its end */
    handle_t give = HANDLE_INVALID;
    uint64_t bytes = f.size;
    struct serve_answer ans = { 0 };
    status_t st = serve_share_until(ch, now() + SOON, port, name, &give);
    if (st == OK)
        st = give_file(give, &f, &ans);   /* f is given away, whatever happens */
    else
        file_close(&f);
    if (give)
        jam_handle_close(give);
    if (st == OK)
        st = ans.status;
    if (st != OK) {
        sh_tty("serve: %s on port %u: %s\n", abs, port, why(st));
        return 1;
    }
    sh_say("serve: serving %s (%s) on port %u, in the background (serve stop %u ends it)\n"
           "  on the Mac: curl http://%s:%u/ -o %s\n", abs, sh_human(bytes, size, sizeof(size)),
           ans.port, ans.port, address(a), ans.port, sh_basename(abs));
    return 0;
}

/* argv[i] as a port, or 0 (said). */
static uint16_t port_arg(const char *s)
{
    uint64_t v = 0;
    if (!sh_parse_u64(s, &v) || v < SERVE_PORT_MIN || v > 65535) {
        sh_tty("serve: %s: a port is %u..65535\n", s, SERVE_PORT_MIN);
        return 0;
    }
    return (uint16_t)v;
}

SH_CMD(serve)
{
    bool stopping = argc >= 2 && !strcmp(argv[1], "stop");
    if (argc > 3 || (stopping && argc > 3)) {
        sh_tty("usage: serve [<file> [port] | stop [port]]   (port 8080 unless given)\n");
        return 2;
    }
    handle_t ch = svc_get(SVC_SERVE);
    if (!ch) {
        sh_tty("serve: there is no file server (init didn't start bin/serve)\n");
        return 1;
    }
    if (argc == 1)
        return list(ch);
    uint16_t port = argc == 3 ? port_arg(argv[2]) : stopping ? 0 : SERVE_PORT_DEFAULT;
    if (argc == 3 && !port)
        return 2;
    if (stopping)
        return stop(ch, port);
    return start(ch, argv[1], port);
}
