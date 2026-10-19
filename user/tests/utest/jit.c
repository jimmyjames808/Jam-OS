/* utest: JIT memory (vmo_make_jit, docs/CR-PLAN.md section 12), the one
 * exception to W^X, from user space: a program holding RIGHT_ROOT_JIT
 * makes a JIT VMO, maps it writable and executable, writes code into it,
 * runs it, patches it in place and runs it again; an execute-only
 * duplicate runs the same bytes and can't write them, a write-only one
 * can't run them, and a duplicate passed on through a channel keeps
 * exactly its rights. Every other VMO keeps W^X. Needs the JIT power,
 * SR_JIT (init's run and the shell give it for our list's `right jit`;
 * our root never has it); without it the test checks the root's refusal
 * and skips the rest. The kernel's half of the rules is in
 * kernel/test/test_vmo_jit.c. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include <posixfds.h>
#include "utest.h"

#define PG  PAGE_SIZE
#define RW  (VMAR_READ | VMAR_WRITE)
#define RX  (VMAR_READ | VMAR_EXEC)
#define RWX (VMAR_READ | VMAR_WRITE | VMAR_EXEC)

typedef int (*ret_fn)(void);

/* "mov eax, imm32; ret" at code: a function returning n. */
static void emit_ret(volatile uint8_t *code, int32_t n)
{
    code[0] = 0xb8;
    memcpy((uint8_t *)code + 1, &n, 4);
    code[5] = 0xc3;
}

static ret_fn fn_at(uint64_t addr)
{
    return (ret_fn)(uintptr_t)addr;
}

static rights_t rights_of(handle_t h)
{
    struct handle_info hi;
    return jam_handle_info(h, &hi) == OK ? hi.rights : 0;
}

bool t_jit(void)
{
    handle_t vmar = startup_handle(SR_SELF_VMAR), root = startup_handle(SR_RESOURCE);
    handle_t jit = startup_handle(SR_JIT), power, v, v2, x;
    uint64_t a = 0;
    /* A plain VMO: W+X refused by map and protect alike. */
    CHECK_ST(jam_vmo_create(PG, 0, &v), OK);
    CHECK_ST(jam_vmar_map(vmar, v, 0, PG, RW, &a), OK);
    CHECK_ST(jam_vmar_protect(vmar, a, PG, RWX), ERR_INVALID_ARGS);
    CHECK_ST(jam_vmar_unmap(vmar, a, PG), OK);
    if (root) {   /* our root never has the power: refused, and v2 is gone */
        CHECK_ST(jam_handle_duplicate(v, RIGHT_SAME, &v2), OK);
        CHECK_ST(jam_vmo_make_jit(v2, root, &x), ERR_ACCESS_DENIED);
    }
    if (!jit || jam_handle_duplicate(jit, RIGHT_DUPLICATE | RIGHT_ROOT_JIT, &power) != OK) {
        CHECK_ST(jam_handle_close(v), OK);
        printf("utest: %s: no JIT power (SR_JIT): skipped\n", utest_cur);
        return true;
    }
    rights_t before = rights_of(v);
    CHECK_ST(jam_vmo_make_jit(v, power, &x), OK);
    CHECK_ST(jam_handle_close(v), ERR_BAD_HANDLE);   /* the handle came back as x */
    CHECK_EQ(rights_of(x), before | RIGHT_EXEC);

    /* Write code, run it, patch it in place, run it again. */
    CHECK_ST(jam_vmar_map(vmar, x, 0, PG, RWX, &a), OK);
    volatile uint8_t *code = (volatile uint8_t *)(uintptr_t)a;
    emit_ret(code, 42);
    CHECK_EQ(fn_at(a)(), 42);
    emit_ret(code, 43);
    CHECK_EQ(fn_at(a)(), 43);
    struct process_info pi;
    CHECK_ST(jam_process_get_info(startup_handle(SR_SELF_PROCESS), &pi), OK);
    CHECK(pi.flags & PROCESS_FLAG_JIT);
    /* Protect back and forth: run-only, then writable and executable again. */
    CHECK_ST(jam_vmar_protect(vmar, a, PG, RX), OK);
    CHECK_EQ(fn_at(a)(), 43);
    CHECK_ST(jam_vmar_protect(vmar, a, PG, RWX), OK);
    emit_ret(code, 44);
    CHECK_EQ(fn_at(a)(), 44);

    /* Execute-only: runs the same bytes, never writes them. */
    handle_t xo, wo;
    CHECK_ST(jam_handle_duplicate(x, RIGHTS_BASIC | RIGHT_READ | RIGHT_MAP | RIGHT_EXEC, &xo), OK);
    uint64_t b = 0;
    CHECK_ST(jam_vmar_map(vmar, xo, 0, PG, RWX, &b), ERR_ACCESS_DENIED);
    CHECK_ST(jam_vmar_map(vmar, xo, 0, PG, RW, &b), ERR_ACCESS_DENIED);
    CHECK_ST(jam_vmar_map(vmar, xo, 0, PG, RX, &b), OK);
    CHECK_EQ(fn_at(b)(), 44);
    emit_ret(code, 45);   /* through the writable view */
    CHECK_EQ(fn_at(b)(), 45);
    CHECK_ST(jam_vmar_protect(vmar, b, PG, RWX), ERR_ACCESS_DENIED);
    CHECK_ST(jam_vmo_write(xo, 0, "\xc3", 1), ERR_ACCESS_DENIED);
    CHECK_ST(jam_vmar_unmap(vmar, b, PG), OK);
    /* Write without execute: writes, never runs. */
    CHECK_ST(jam_handle_duplicate(x, RIGHTS_BASIC | RIGHT_READ | RIGHT_MAP | RIGHT_WRITE, &wo),
             OK);
    CHECK_ST(jam_vmar_map(vmar, wo, 0, PG, RX, &b), ERR_ACCESS_DENIED);
    CHECK_ST(jam_vmar_map(vmar, wo, 0, PG, RW, &b), OK);
    CHECK_ST(jam_vmar_protect(vmar, b, PG, RX), ERR_ACCESS_DENIED);
    emit_ret((volatile uint8_t *)(uintptr_t)b, 46);
    CHECK_EQ(fn_at(a)(), 46);
    CHECK_ST(jam_vmar_unmap(vmar, b, PG), OK);

    /* Passed on (a channel to ourselves): exactly the rights sent. */
    handle_t c0, c1, got = 0;
    rights_t xo_rights = rights_of(xo);
    CHECK_ST(jam_channel_create(&c0, &c1), OK);
    CHECK_ST(jam_channel_write(c0, "j", 1, &xo, 1), OK);
    uint8_t byte;
    uint32_t nb = 0, nh = 0;
    struct channel_read_args ra = {
        .h = c1, .rd_cap = 1, .rd_bytes = (uint64_t)(uintptr_t)&byte,
        .rd_actual = (uint64_t)(uintptr_t)&nb, .rd_handles = (uint64_t)(uintptr_t)&got,
        .rd_hcap = 1, .rd_hactual = (uint64_t)(uintptr_t)&nh,
    };
    CHECK_ST(jam_channel_read(&ra), OK);
    CHECK_EQ(nh, 1);
    CHECK_EQ(rights_of(got), xo_rights);
    CHECK_ST(jam_vmar_map(vmar, got, 0, PG, RWX, &b), ERR_ACCESS_DENIED);
    CHECK_ST(jam_vmar_map(vmar, got, 0, PG, RX, &b), OK);
    CHECK_EQ(fn_at(b)(), 46);
    CHECK_ST(jam_vmar_unmap(vmar, b, PG), OK);

    /* The old ways to code refuse it: no child, no seal (x is gone). */
    handle_t c, s;
    CHECK_ST(jam_vmo_create_child(x, VMO_CHILD_SNAPSHOT, 0, PG, &c), ERR_NOT_SUPPORTED);
    CHECK_ST(jam_vmar_unmap(vmar, a, PG), OK);
    CHECK_ST(jam_handle_close(got), OK);
    CHECK_ST(jam_handle_close(wo), OK);
    CHECK_ST(jam_handle_close(c0), OK);
    CHECK_ST(jam_handle_close(c1), OK);
    handle_t vmex;
    if (jam_handle_duplicate(root, RIGHTS_BASIC | RIGHT_ROOT_VMEX, &vmex) == OK) {
        CHECK_ST(jam_vmo_make_exec(x, vmex, &s), ERR_BAD_STATE);
        CHECK_ST(jam_handle_close(vmex), OK);
    } else {
        CHECK_ST(jam_handle_close(x), OK);
    }
    /* A VMO with a page in it, or mapped, can't become JIT memory. */
    CHECK_ST(jam_vmo_create(PG, 0, &v), OK);
    CHECK_ST(jam_vmo_write(v, 0, "a", 1), OK);
    CHECK_ST(jam_vmo_make_jit(v, power, &x), ERR_BAD_STATE);
    CHECK_ST(jam_vmo_create(PG, 0, &v), OK);
    CHECK_ST(jam_vmar_map(vmar, v, 0, PG, RW, &a), OK);
    CHECK_ST(jam_vmo_make_jit(v, power, &x), ERR_BAD_STATE);
    CHECK_ST(jam_vmar_unmap(vmar, a, PG), OK);
    CHECK_ST(jam_handle_close(power), OK);
    return true;
}

/* A program started from JIT memory (spawn given a JIT VMO's handle,
 * docs/CR-REVIEW-JIT.md finding 1): it runs, marked, from the bytes its
 * starter wrote; but it holds no handle that writes them, so its code is
 * never writable: the child (jit_child, "jit-text") can't protect the page
 * it runs on writable and executable, which would let a program without
 * the power write its starter's JIT memory. */
bool t_jit_spawn(void)
{
    handle_t jit = startup_handle(SR_JIT), power, v, x, job, proc;
    if (!jit || jam_handle_duplicate(jit, RIGHT_DUPLICATE | RIGHT_ROOT_JIT, &power) != OK) {
        printf("utest: %s: no JIT power (SR_JIT): skipped\n", utest_cur);
        return true;
    }
    const struct bootfs_view *fs;
    const void *data;
    uint64_t size;
    CHECK_ST(bootfs_default(&fs), OK);
    CHECK_ST(bootfs_lookup(fs, "bin/utest", &data, &size), OK);
    CHECK_ST(jam_vmo_create(size, 0, &v), OK);
    CHECK_ST(jam_vmo_make_jit(v, power, &x), OK);
    CHECK_ST(jam_handle_close(power), OK);
    CHECK_ST(jam_vmo_write(x, 0, data, size), OK);
    CHECK_ST(jam_job_create(startup_handle(SR_JOB), 0, &job), OK);
    const char *argv[] = { "utest", "jit-text" };
    struct spawn_args a = {
        .path = "bin/utest", .name = "utest-jit-text", .vmo = x, .size = size, .argc = 2,
        .argv = argv, .job = job,
    };
    CHECK_ST(spawn(&a, &proc), OK);
    CHECK_ST(jam_handle_close(x), OK);   /* the child's mappings keep the VMO */
    struct process_info info;
    CHECK_ST(spawn_wait(proc, 10 * NS_PER_S, &info), OK);
    CHECK(!info.killed);
    CHECK_EQ(info.exit_code, 0);
    CHECK_ST(jam_handle_close(proc), OK);
    CHECK_ST(jam_handle_close(job), OK);
    return true;
}

/* bin/jittest in mode `mode` with two SR_JIT handles (duplicates of ours):
 * its exit code, or -1. */
static int jittest_two_powers(handle_t jit, const char *mode)
{
    handle_t job, proc;
    struct spawn_handle x[2] = { { SR_JIT, HANDLE_INVALID }, { SR_JIT, HANDLE_INVALID } };
    if (new_job(&job) != OK)
        return -1;
    for (unsigned i = 0; i < 2; i++)
        if (jam_handle_duplicate(jit, RIGHT_SAME, &x[i].h) != OK) {
            if (i)
                jam_handle_close(x[0].h);
            jam_handle_close(job);
            return -1;
        }
    struct posixfds_out ho = { .cwd = "/" };
    const char *argv[] = { "jittest", mode };
    struct spawn_args a = { .path = "bin/jittest", .argc = 2, .argv = argv, .job = job,
                            .extra = x, .nextra = 2, .handover = &ho,
                            .stack_size = 8ull << 20 };
    struct process_info info;
    int code = -1;
    if (spawn(&a, &proc) == OK) {
        if (spawn_wait(proc, 10 * NS_PER_S, &info) == OK && !info.killed)
            code = (int)info.exit_code;
        jam_handle_close(proc);
    }
    jam_handle_close(job);
    return code;
}

/* PR_SET_MDWE gives the power up whole (docs/CR-REVIEW-JIT.md, CJR2
 * finding 4): bin/jittest started with two SR_JIT handles holds the power
 * (mode "power": 0), and after PR_SET_MDWE holds none (mode "giveup": 1;
 * before the fix libc closed the first handle only, so MAP_JIT still
 * worked with the second and PR_GET_MDWE said not in force: 0). */
bool t_jit_mdwe(void)
{
    handle_t jit = startup_handle(SR_JIT);
    if (!jit) {
        printf("utest: %s: no JIT power (SR_JIT): skipped\n", utest_cur);
        return true;
    }
    CHECK_EQ(jittest_two_powers(jit, "power"), 0);
    CHECK_EQ(jittest_two_powers(jit, "giveup"), 1);
    return true;
}

/* "jit-text", started by t_jit_spawn from a JIT VMO: 0 if it runs marked,
 * its image may run and not write, and its code can't be made writable;
 * else what failed. */
int jit_child(int argc, char **argv)
{
    (void)argc;
    if (strcmp(argv[1], "jit-text"))
        return 127;
    struct process_info pi;
    if (jam_process_get_info(startup_handle(SR_SELF_PROCESS), &pi) != OK ||
        !(pi.flags & PROCESS_FLAG_JIT))
        return 2;   /* runs JIT memory, unmarked */
    handle_t img = startup_handle(SR_SELF_IMAGE);
    if (!img || (rights_of(img) & (RIGHT_WRITE | RIGHT_EXEC)) != RIGHT_EXEC)
        return 3;
    uint64_t page = (uint64_t)(uintptr_t)&jit_child & ~(uint64_t)(PG - 1);
    status_t st = jam_vmar_protect(startup_handle(SR_SELF_VMAR), page, PG, RWX);
    if (st == OK) {
        printf("utest: jit-text: our code is writable and executable\n");
        return 1;
    }
    return st == ERR_ACCESS_DENIED ? 0 : 4;
}
