/* utest: libos's spawn of a verified program (vmo_make_exec_verified),
 * with utest itself as the pager behind it.
 *
 * t_spawn_verified: bin/utest's bytes served page by page by a pager of
 * ours (a thread answers its requests from the boot image), sealed with
 * a table of each page's SHA-256. With the right table the program runs
 * (spawn read its headers and its writable segments through vmo_read, the
 * child faulted in its code). With page 0's hash wrong, spawn fails
 * ERR_IO and we live: spawn never maps the program here, so the bad page
 * is a failed read, not a fault that would end this process (the kernel
 * kills whoever touches a page that doesn't match). Needs RIGHT_ROOT_VMEX
 * and RIGHT_ROOT_PAGER on our root (init's run: TEST_ROOT); without them
 * it is skipped.
 *
 * t_copy_from_lost_page: a system call copying from a mapping of a pager
 * VMO whose pager is gone (detached: a pulled stick) fails ERR_IO, not
 * ERR_INVALID_ARGS (a bad pointer), and we live: channel_write (a POSIX
 * pipe's write) and vmo_write. Needs RIGHT_ROOT_PAGER. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include <sha256.h>
#include "utest.h"

#define SV_ANSWER_NS (10ull * NS_PER_S)   /* the pager's answer time */
#define SV_POLL_NS   (20 * NS_PER_MS)     /* the server looks at `stop` this often */

/* The pager's server thread and what it serves. */
struct sv {
    handle_t       pager;
    const uint8_t *data;    /* the program's bytes (the boot image's mapping) */
    uint64_t       size;    /* how many */
    volatile bool  stop;    /* set by the test: the thread ends */
    volatile bool  done;    /* set by the thread as it ends */
};

static uint8_t sv_stack[16384];
static uint8_t sv_page[PAGE_SIZE];   /* the last page, zero past the end */

/* Answer one read request: the program's pages, the last zero-padded. */
static void sv_supply(struct sv *s, const struct pager_request *r)
{
    for (uint64_t at = r->offset; at < r->offset + r->len; at += PAGE_SIZE) {
        const uint8_t *src = s->data + at;
        if (at >= s->size) {
            (void)jam_pager_op(s->pager, r->key, PAGER_OP_FAIL, at, r->offset + r->len - at,
                               ERR_OUT_OF_RANGE);   /* past the program: never asked */
            return;
        }
        if (s->size - at < PAGE_SIZE) {
            memset(sv_page, 0, sizeof(sv_page));
            memcpy(sv_page, src, s->size - at);
            src = sv_page;
        }
        /* A page that doesn't match is ERR_IO here: the kernel failed its
         * waiters already. */
        (void)jam_pager_supply(s->pager, r->key, at, src, PAGE_SIZE);
    }
}

static void sv_serve(void *arg)
{
    struct sv *s = arg;
    while (!s->stop) {
        signals_t seen;
        if (jam_object_wait_one(s->pager, SIG_READABLE, now() + SV_POLL_NS, &seen) != OK)
            continue;
        struct pager_request r;
        if (jam_pager_next(s->pager, 0, &r) != OK)
            continue;
        if (r.kind == PAGER_REQ_READ)
            sv_supply(s, &r);
        else if (r.kind == PAGER_REQ_GONE)
            (void)jam_pager_op(s->pager, r.key, PAGER_OP_DETACH, 0, 0, 0);
    }
    s->done = true;
}

/* A table VMO for the program: each page's SHA-256 (the last zero-padded),
 * page `bad`'s with one bit changed (bad >= the pages: none). */
static status_t make_table(const struct sv *s, uint64_t bad, handle_t *out)
{
    uint64_t pages = (s->size + PAGE_SIZE - 1) / PAGE_SIZE;
    handle_t t;
    status_t st = jam_vmo_create(pages * VMO_VERIFIED_HASH_BYTES, 0, &t);
    if (st != OK)
        return st;
    for (uint64_t i = 0; st == OK && i < pages; i++) {
        uint8_t page[PAGE_SIZE] = { 0 }, digest[SHA256_BYTES];
        uint64_t n = s->size - i * PAGE_SIZE < PAGE_SIZE ? s->size - i * PAGE_SIZE : PAGE_SIZE;
        memcpy(page, s->data + i * PAGE_SIZE, n);
        sha256(page, sizeof(page), digest);
        digest[0] ^= i == bad ? 1 : 0;
        st = jam_vmo_write(t, i * VMO_VERIFIED_HASH_BYTES, digest, sizeof(digest));
    }
    if (st != OK) {
        jam_handle_close(t);
        return st;
    }
    *out = t;
    return OK;
}

/* "utest exit7" spawned from a fresh pager VMO (under key, a new one each
 * time: an old key stays taken until its GONE is answered) sealed with a
 * table whose page `bad` is wrong (none if bad is past the end): spawn's
 * status. */
static status_t run_verified(struct sv *s, handle_t vmex, uint64_t key, uint64_t bad,
                             bool *exited7)
{
    handle_t vmo, table, x, job, proc;
    uint64_t len = (s->size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    *exited7 = false;
    status_t st = jam_pager_create_vmo(s->pager, key, len, 0, &vmo);
    if (st != OK)
        return st;
    st = make_table(s, bad, &table);
    if (st != OK) {
        jam_handle_close(vmo);
        return st;
    }
    st = jam_vmo_make_exec_verified(vmo, vmex, table, 0, &x);   /* vmo is gone either way */
    jam_handle_close(table);
    if (st != OK)
        return st;
    st = new_job(&job);
    if (st == OK) {
        const char *argv[] = { "utest", "exit7" };
        struct spawn_args a = { .path = "utest-verified", .vmo = x, .size = s->size,
                                .argc = 2, .argv = argv, .job = job };
        st = spawn(&a, &proc);
        if (st == OK)
            *exited7 = ns_child_exits(proc, 7);
        jam_handle_close(job);
    }
    jam_handle_close(x);
    return st;
}

bool t_spawn_verified(void)
{
    static struct sv s;
    handle_t root = startup_handle(SR_RESOURCE), vmex = HANDLE_INVALID, thread;
    s = (struct sv){ .pager = HANDLE_INVALID };
    if (!root || jam_handle_duplicate(root, RIGHT_ROOT_VMEX, &vmex) != OK ||
        jam_pager_create(root, 4096, SV_ANSWER_NS, 0, &s.pager) != OK) {
        if (vmex)
            jam_handle_close(vmex);
        printf("utest: %s: no RIGHT_ROOT_VMEX and RIGHT_ROOT_PAGER on our root: skipped\n",
               utest_cur);
        return true;
    }
    const struct bootfs_view *fs;
    const void *data;
    CHECK_ST(bootfs_default(&fs), OK);
    CHECK_ST(bootfs_lookup(fs, "bin/utest", &data, &s.size), OK);
    s.data = data;
    CHECK(s.size <= VMO_VERIFIED_MAX);
    CHECK_ST(thread_spawn("verified pager", sv_serve, &s, sv_stack, sizeof(sv_stack), &thread),
             OK);
    bool exited7;
    /* The approved bytes: it runs. */
    CHECK_ST(run_verified(&s, vmex, 1, UINT64_MAX, &exited7), OK);
    CHECK(exited7);
    /* Page 0 (the ELF header) not the approved one: spawn's read of it
     * fails, and we are still here to see it. */
    CHECK_ST(run_verified(&s, vmex, 2, 0, &exited7), ERR_IO);
    s.stop = true;
    for (unsigned i = 0; i < 500 && !s.done; i++)
        jam_sleep_until(now() + 10 * NS_PER_MS);
    CHECK(s.done);
    jam_handle_close(thread);
    jam_handle_close(s.pager);
    jam_handle_close(vmex);
    return true;
}

bool t_copy_from_lost_page(void)
{
    handle_t root = startup_handle(SR_RESOURCE), pager, vmo, a, b, sink;
    uint64_t addr = 0;
    if (!root || jam_pager_create(root, 64, SV_ANSWER_NS, 0, &pager) != OK) {
        printf("utest: %s: no RIGHT_ROOT_PAGER on our root: skipped\n", utest_cur);
        return true;
    }
    CHECK_ST(jam_pager_create_vmo(pager, 1, 4 * PAGE_SIZE, 0, &vmo), OK);
    CHECK_ST(jam_vmar_map(startup_handle(SR_SELF_VMAR), vmo, 0, 4 * PAGE_SIZE, VMAR_READ,
                          &addr), OK);
    CHECK_ST(jam_pager_op(pager, 1, PAGER_OP_DETACH, 0, 0, 0), OK);   /* the stick pulled */
    const void *lost = (const void *)(uintptr_t)(addr + PAGE_SIZE);
    CHECK_ST(jam_channel_create(&a, &b), OK);
    CHECK_ST(jam_channel_write(a, lost, 64, NULL, 0), ERR_IO);
    CHECK_ST(jam_channel_write(a, (const void *)8, 64, NULL, 0), ERR_INVALID_ARGS);
    CHECK_ST(jam_vmo_create(PAGE_SIZE, 0, &sink), OK);
    CHECK_ST(jam_vmo_write(sink, 0, lost, 64), ERR_IO);
    CHECK_ST(jam_vmar_unmap(startup_handle(SR_SELF_VMAR), addr, 4 * PAGE_SIZE), OK);
    jam_handle_close(sink);
    jam_handle_close(a);
    jam_handle_close(b);
    jam_handle_close(vmo);
    jam_handle_close(pager);
    return true;
}
