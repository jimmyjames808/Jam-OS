/* utest's child modes: "utest <mode> [args]" runs one of these instead of
 * the test suite. The suite (and the kernel's own tests) spawn them to get
 * a second process that behaves in one particular way: crashes, blocks in
 * a call, allocates until its job says no, echoes messages... Each returns
 * the exit code the spawner checks for. */
#include <os.h>
#include "utest.h"

/* Crash on purpose: a NULL write. The kernel must kill us (and only us). */
static int nullderef(void)
{
    *(volatile int *)0 = 1;
    return 99;   /* not reached */
}

/* Execute a data page (heap, mapped RW so never executable): killed. */
static int execdata(void)
{
    uint8_t *code = malloc(64);
    if (!code)
        return 98;
    code[0] = 0xc3;   /* ret */
    ((void (*)(void))code)();
    return 99;
}

/* Spin in user mode until killed (a kill must reach a thread that never
 * enters the kernel on its own). */
static int spin(void)
{
    for (volatile uint64_t i = 0;; i++)
        ;
    return 99;
}

/* Echo server on SR_USER: every message goes back as it came, handles
 * included, until the other end closes. */
static int echo(void)
{
    handle_t ch = startup_handle(SR_USER);
    static uint8_t buf[CHANNEL_MSG_MAX];
    handle_t hs[8];
    for (;;) {
        uint32_t nb = 0, nh = 0;
        struct channel_read_args a = {
            .h = ch, .bytes_cap = sizeof(buf), .bytes = (uint64_t)(uintptr_t)buf,
            .actual_bytes = (uint64_t)(uintptr_t)&nb, .handles = (uint64_t)(uintptr_t)hs,
            .handles_cap = 8, .actual_handles = (uint64_t)(uintptr_t)&nh,
        };
        status_t st = jam_channel_read(&a);
        if (st == ERR_SHOULD_WAIT) {
            signals_t seen;
            st = jam_object_wait_one(ch, SIG_READABLE | SIG_PEER_CLOSED, DEADLINE_NEVER, &seen);
            if (st != OK)
                return 2;
            continue;
        }
        if (st == ERR_PEER_CLOSED)
            return 0;
        if (st != OK)
            return 3;
        st = jam_channel_write(ch, buf, nb, hs, nh);
        if (st != OK && st != ERR_PEER_CLOSED)
            return 4;
    }
}

static void sleep_forever(void *arg)
{
    (void)arg;
    jam_nanosleep(DEADLINE_NEVER);
}

/* Hold some of everything, then block in channel_call on SR_USER, whose
 * other end never answers: the spawner kills us there and checks that
 * every page, handle and thread comes back. */
static int caller(void)
{
    handle_t ch = startup_handle(SR_USER);
    uint8_t *mem = malloc(64 * 1024);
    if (!mem)
        return 6;
    memset(mem, 0x5a, 64 * 1024);   /* commit heap pages */
    handle_t ev, th;
    if (jam_event_create(&ev) != OK)
        return 7;
    static uint8_t stack[16384] __attribute__((aligned(16)));
    if (thread_spawn("sleeper", sleep_forever, NULL, stack, sizeof(stack), &th) != OK)
        return 8;
    uint8_t req[16] = { 0 }, rep[16];
    uint32_t ra = 0;
    struct channel_call_args a = {
        .h = ch, .wn = sizeof(req), .wbytes = (uint64_t)(uintptr_t)req,
        .rcap = sizeof(rep), .rbytes = (uint64_t)(uintptr_t)rep,
        .ractual = (uint64_t)(uintptr_t)&ra, .rhcap = 4, .deadline_ns = DEADLINE_NEVER,
    };
    handle_t rh[4];
    a.rh = (uint64_t)(uintptr_t)rh;
    status_t st = jam_channel_call(&a);
    printf("utest-caller: channel_call returned %s (it never should)\n", status_str(st));
    return 5;
}

/* Allocate through VMO commits until the job's page limit refuses: the
 * refusal must be ERR_NO_MEMORY, and we exit normally with 42. */
static int hog(void)
{
    for (unsigned i = 0; i < 100000; i++) {
        handle_t v;
        status_t st = jam_vmo_create(256 * 1024, 0, HANDLE_INVALID, &v);
        if (st == OK)
            st = jam_vmo_commit(v, 0, 256 * 1024);
        if (st == ERR_NO_MEMORY)
            return 42;
        if (st != OK) {
            printf("utest-hog: %s after %u VMOs\n", status_str(st), i);
            return 43;
        }
    }
    return 41;   /* never refused */
}

/* The same through page faults: touching pages past the limit can't be
 * refused politely, so the kernel kills us ("out of memory"). */
static int hogfault(void)
{
    handle_t v, vmar = startup_handle(SR_SELF_VMAR);
    uint64_t size = 64ull << 20, addr = 0;
    if (jam_vmo_create(size, 0, HANDLE_INVALID, &v) != OK ||
        jam_vmar_map(vmar, v, 0, size, VMAR_READ | VMAR_WRITE, &addr) != OK)
        return 43;
    for (uint64_t off = 0; off < size; off += 4096)
        *(volatile uint8_t *)(uintptr_t)(addr + off) = 1;
    return 41;   /* never stopped */
}

static void nothing(void *arg)
{
    (void)arg;
}

/* With a job thread limit of 1: a second thread can't start. */
static int threads2(void)
{
    static uint8_t stack[8192] __attribute__((aligned(16)));
    handle_t th;
    status_t st = thread_spawn("second", nothing, NULL, stack, sizeof(stack), &th);
    return st == ERR_NO_RESOURCES ? 44 : 40;
}

/* With a small job handle limit: events until ERR_NO_RESOURCES. */
static int handles(void)
{
    for (unsigned i = 0; i < 1000; i++) {
        handle_t h;
        status_t st = jam_event_create(&h);
        if (st == ERR_NO_RESOURCES)
            return i > 0 ? 45 : 47;
        if (st != OK)
            return 46;
    }
    return 41;
}

/* With a small job message-byte limit: queue 1 KiB messages on our own
 * channel until the sender's (our) job refuses with ERR_NO_MEMORY. */
static int msgs(void)
{
    handle_t a, b;
    if (jam_channel_create(&a, &b) != OK)
        return 48;
    static uint8_t buf[1024];
    for (unsigned i = 0; i < 1000; i++) {
        status_t st = jam_channel_write(a, buf, sizeof(buf), NULL, 0);
        if (st == ERR_NO_MEMORY)
            return i > 0 ? 46 : 47;
        if (st != OK)
            return 49;
    }
    return 41;
}

/* With a small job message-byte limit: port packets, then port bindings,
 * are charged to us too, so our job (not the port's per-port caps of 4096
 * each) stops us with ERR_NO_MEMORY. 47 if both were refused that way,
 * well below the port's caps. */
static int ports(void)
{
    handle_t port, ev;
    if (jam_port_create(&port) != OK || jam_event_create(&ev) != OK)
        return 2;
    struct port_packet pk = { .key = 1, .type = PORT_PACKET_USER }, out;
    unsigned n = 0, b = 0;
    status_t st;
    while ((st = jam_port_queue(port, &pk)) == OK)
        if (++n >= 4096)
            return 41;
    if (st != ERR_NO_MEMORY)
        return 43;
    while (jam_port_wait(port, 0, &out) == OK)
        ;   /* give the budget back for the bindings */
    while ((st = jam_port_bind(port, ev, b, SIG_SIGNALED, PORT_BIND_PERSISTENT)) == OK)
        if (++b >= 4096)
            return 42;
    if (st != ERR_NO_MEMORY)
        return 44;
    return n > 0 && b > 0 ? 47 : 45;
}

/* Start a spinning grandchild in a job of its own under ours and exit at
 * once, leaving it an orphan nobody holds a handle to: only killing our
 * job (job_kill) can get rid of it. */
static int orphan(void)
{
    handle_t sub, proc;
    if (jam_job_create(startup_handle(SR_JOB), 0, &sub) != OK)
        return 60;
    const char *argv[] = { "utest", "spin" };
    struct spawn_args a = {
        .path = "bin/utest", .name = "utest-orphaned", .argc = 2, .argv = argv, .job = sub,
    };
    if (spawn(&a, &proc) != OK)
        return 61;
    return 0;   /* our handles (proc, sub) close as we go */
}

/* The main thread leaves; the process lives on in its second thread,
 * which exits the process with 11. */
static void finisher(void *arg)
{
    (void)arg;
    jam_nanosleep(now() + 20000000ull);
    jam_process_exit(11);
}

static int main_exits(void)
{
    static uint8_t stack[8192] __attribute__((aligned(16)));
    handle_t th;
    if (thread_spawn("finisher", finisher, NULL, stack, sizeof(stack), &th) != OK)
        return 12;
    jam_thread_exit();
}

/* Check what userboot / spawn gave us: argv and every startup handle, with
 * the rights we expect. 0 if all is well. */
static int startup(int argc, char **argv)
{
    if (argc < 3 || strcmp(argv[2], "hello"))
        return 20;
    static const uint32_t roles[] = { SR_SELF_PROCESS, SR_SELF_VMAR, SR_SELF_THREAD, SR_JOB,
                                      SR_BOOTFS };
    for (unsigned i = 0; i < sizeof(roles) / sizeof(roles[0]); i++)
        if (startup_handle(roles[i]) == HANDLE_INVALID)
            return 21 + (int)i;
    handle_t fs = startup_handle(SR_BOOTFS);
    uint8_t byte = 0;
    if (jam_vmo_write(fs, 0, &byte, 1) != ERR_ACCESS_DENIED)
        return 30;   /* bootfs must never be writable */
    struct process_info pi;
    if (jam_process_get_info(startup_handle(SR_SELF_PROCESS), &pi) != OK ||
        pi.state != PROCESS_RUNNING || pi.threads != 1)
        return 31;
    return 0;
}

/* Try to lift our own job's page limit through SR_JOB, then commit 1 MiB
 * (kernel/test/test_quota.c, quota_child_cannot_raise_own_job_limit). 50
 * if the kernel refused the raise (right: SR_JOB has no RIGHT_MANAGE), 0 if
 * both worked (the review's R4 bug), 51 if the raise "worked" but the
 * commit was still refused. */
static int raise_own_limit(void)
{
    if (jam_job_set_limit(startup_handle(SR_JOB), JOB_LIMIT_PAGES, JOB_NO_LIMIT) != OK)
        return 50;
    handle_t v;
    if (jam_vmo_create(1 << 20, 0, HANDLE_INVALID, &v) != OK)
        return 52;
    status_t st = jam_vmo_commit(v, 0, 1 << 20);
    jam_handle_close(v);
    return st == OK ? 0 : 51;
}

/* The shell's test (tools/shell-tests/cmds.txt): print the environment
 * and the arguments, which `run` passes (and, in a pipe, through the
 * SR_STDOUT channel). */
static int print_env(int argc, char **argv)
{
    for (char **e = environ; *e; e++)
        printf("env: %s\n", *e);
    for (int i = 2; i < argc; i++)
        printf("arg: %s\n", argv[i]);
    return 0;
}

int child_main(int argc, char **argv)
{
    const char *m = argv[1];
    if (!strcmp(m, "nullderef"))  return nullderef();
    if (!strcmp(m, "execdata"))   return execdata();
    if (!strcmp(m, "spin"))       return spin();
    if (!strcmp(m, "echo"))       return echo();
    if (!strcmp(m, "caller"))     return caller();
    if (!strcmp(m, "hog"))        return hog();
    if (!strcmp(m, "hogfault"))   return hogfault();
    if (!strcmp(m, "threads2"))   return threads2();
    if (!strcmp(m, "handles"))    return handles();
    if (!strcmp(m, "msgs"))       return msgs();
    if (!strcmp(m, "ports"))      return ports();
    if (!strcmp(m, "orphan"))     return orphan();
    if (!strcmp(m, "main-exits")) return main_exits();
    if (!strcmp(m, "startup"))    return startup(argc, argv);
    if (!strcmp(m, "exit7"))      return 7;
    if (!strcmp(m, "env"))        return print_env(argc, argv);
    if (!strcmp(m, "raise-own-limit")) return raise_own_limit();
    if (!strncmp(m, "bench-", 6)) return bench_child(argc, argv);
    printf("utest: unknown mode \"%s\"\n", m);
    return 127;
}
