/* utest: JIT memory (vmo_make_jit, docs/CR-PLAN.md section 12), the one
 * exception to W^X, from user space: a program holding RIGHT_ROOT_JIT
 * makes a JIT VMO, maps it writable and executable, writes code into it,
 * runs it, patches it in place and runs it again; an execute-only
 * duplicate runs the same bytes and can't write them, a write-only one
 * can't run them, and a duplicate passed on through a channel keeps
 * exactly its rights. Every other VMO keeps W^X. Needs RIGHT_ROOT_JIT on
 * our root (init's run: TEST_ROOT; the shell: our list's `right jit`);
 * without it the test checks the refusal and skips the rest. The kernel's
 * half of the rules is in kernel/test/test_vmo_jit.c. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
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
    handle_t power, v, x;
    uint64_t a = 0;
    /* A plain VMO: W+X refused by map and protect alike. */
    CHECK_ST(jam_vmo_create(PG, 0, &v), OK);
    CHECK_ST(jam_vmar_map(vmar, v, 0, PG, RW, &a), OK);
    CHECK_ST(jam_vmar_protect(vmar, a, PG, RWX), ERR_INVALID_ARGS);
    CHECK_ST(jam_vmar_unmap(vmar, a, PG), OK);
    if (!root || jam_handle_duplicate(root, RIGHT_DUPLICATE | RIGHT_ROOT_JIT, &power) != OK) {
        if (root)   /* our root without the power: refused, and v is gone */
            CHECK_ST(jam_vmo_make_jit(v, root, &x), ERR_ACCESS_DENIED);
        else
            CHECK_ST(jam_handle_close(v), OK);
        printf("utest: %s: no RIGHT_ROOT_JIT on our root: skipped\n", utest_cur);
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
