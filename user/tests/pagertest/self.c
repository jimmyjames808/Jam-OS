/* pagertest self: libos's mapped files (fs_map_file, mapped_file_from_vmo,
 * mapped_file_close) over VMOs a program made itself, so they need no
 * mount and no pager: a file of every size class (empty, a byte, a page
 * less one, one page, a page and a byte, several) read through its
 * mapping and hashed, the page tail past the end zero, and the calls'
 * refusals. What a real pager adds (pages that arrive on demand, or
 * don't) is the other commands' to show. */
#include <fs_idl.h>
#include <sha256.h>
#include "pagertest.h"

#define CHECK_PROG "pagertest"
#define CHECK_CUR  cur
#include <check.h>

static const char *cur;   /* the check running, for the failure line */

static uint8_t pattern(uint64_t i)
{
    return (uint8_t)(i * 131 + (i >> 8) * 7 + 5);
}

/* A VMO of `size` bytes of the pattern (whole pages, the rest zero). */
static status_t make_vmo(uint64_t size, handle_t *out)
{
    uint64_t len = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint8_t chunk[PAGE_SIZE];
    handle_t v;
    status_t st = jam_vmo_create(len ? len : PAGE_SIZE, 0, &v);
    for (uint64_t at = 0; st == OK && at < size; at += sizeof(chunk)) {
        uint64_t n = size - at < sizeof(chunk) ? size - at : sizeof(chunk);
        for (uint64_t i = 0; i < n; i++)
            chunk[i] = pattern(at + i);
        st = jam_vmo_write(v, at, chunk, n);
    }
    if (st != OK) {
        if (v)
            jam_handle_close(v);
        return st;
    }
    *out = v;
    return OK;
}

static bool one_size(uint64_t size)
{
    handle_t v = HANDLE_INVALID;
    struct mapped_file mf;
    CHECK_ST(make_vmo(size, &v), OK);
    CHECK_ST(mapped_file_from_vmo(v, size, &mf), OK);
    CHECK_EQ(mf.size, size);
    CHECK_EQ(mf.map_len, (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    CHECK((mf.data != NULL) == (size != 0));
    struct sha256 mapped, want;
    sha256_init(&mapped);
    sha256_init(&want);
    for (uint64_t i = 0; i < size; i++) {
        uint8_t b = pattern(i);
        sha256_add(&want, &b, 1);
    }
    if (size)
        sha256_add(&mapped, mf.data, size);
    uint8_t a[SHA256_BYTES], b[SHA256_BYTES];
    sha256_done(&mapped, a);
    sha256_done(&want, b);
    CHECK(!memcmp(a, b, sizeof(a)));
    if (size % PAGE_SIZE)
        CHECK_EQ(mf.data[size], 0);   /* the last page's tail past the end is zero */
    mapped_file_close(&mf);
    CHECK(!mf.vmo && !mf.data && !mf.size);
    return true;
}

static bool sizes(void)
{
    static const uint64_t list[] = { 0, 1, PAGE_SIZE - 1, PAGE_SIZE, PAGE_SIZE + 1,
                                     3 * PAGE_SIZE, 5 * PAGE_SIZE + 77 };
    for (unsigned i = 0; i < sizeof(list) / sizeof(list[0]); i++)
        if (!one_size(list[i]))
            return false;
    return true;
}

static bool refusals(void)
{
    struct mapped_file mf;
    handle_t v = HANDLE_INVALID, got = HANDLE_INVALID;
    uint64_t size = 0;
    /* a file claimed bigger than its VMO: a broken service. The VMO is consumed. */
    CHECK_ST(make_vmo(PAGE_SIZE, &v), OK);
    CHECK_ST(mapped_file_from_vmo(v, PAGE_SIZE + 1, &mf), ERR_INTERNAL);
    CHECK_ST(jam_handle_close(v), ERR_BAD_HANDLE);
    CHECK_ST(make_vmo(PAGE_SIZE, &v), OK);
    CHECK_ST(mapped_file_from_vmo(v, UINT64_MAX, &mf), ERR_OUT_OF_RANGE);
    CHECK_ST(jam_handle_close(v), ERR_BAD_HANDLE);
    /* fs_map_file: flags are READ or EXEC alone; a closed file has no channel */
    struct jfile f = { 0 };
    CHECK_ST(fs_map_file(&f, 0, &got, &size), ERR_INVALID_ARGS);
    CHECK_ST(fs_map_file(&f, FILE_MAP_READ | FILE_MAP_EXEC, &got, &size), ERR_INVALID_ARGS);
    CHECK_ST(fs_map_file(&f, FILE_MAP_READ, &got, &size), ERR_BAD_STATE);
    CHECK_ST(file_map_path("/", FILE_MAP_READ, &got, &size), ERR_IS_DIR);
    return true;
}

int pagertest_self(void)
{
    static const struct {
        const char *name;
        bool (*run)(void);
    } tests[] = { { "sizes", sizes }, { "refusals", refusals } };
    unsigned failed = 0;
    for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        cur = tests[i].name;
        if (!tests[i].run())
            failed++;
    }
    printf("pagertest: self: %s\n", failed ? "FAILED" : "PASS");
    return failed ? 1 : 0;
}
