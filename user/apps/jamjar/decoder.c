/* jamjar: the cover helper's client. Album covers are decoded by
 * bin/jamcover (user/apps/jamcover, <jamcover.h>), a process of its own
 * that holds nothing but the picture's bytes and its pixels, so a picture
 * that breaks stb_image breaks only that process.
 *
 * cover.c's thread reads a track's tag into the input buffer (a VMO
 * mapped here, given to the helper read-only), finds the picture in it,
 * and calls decoder_decode. The helper is started on the first call, in a
 * job of its own under jamjar's; it writes the pixels into the output VMO,
 * which jamjar never maps: it reads them back with vmo_read, so they are
 * only bytes to it. A call that doesn't answer in JAMCOVER_TIMEOUT, or
 * whose channel closed (the helper crashed), costs that album its cover:
 * the helper's job is killed and the next call starts a new one.
 *
 * One thread calls it (cover.c's; the self-test has no such thread), so
 * nothing here is locked. */
#include <idl/jamcover.h>
#include "jamjar.h"

static struct {
    handle_t       in, out;     /* the two VMOs: ours, kept across helpers */
    uint8_t       *in_map;      /* the input, mapped to write */
    handle_t       job, proc, ch;   /* the helper running (0: none) */
    unsigned       starts;      /* helpers started so far */
    const char    *test_arg;    /* the self-test's --crash or --hang (NULL: none) */
    uint64_t       timeout;     /* ns a decode may take (0: JAMCOVER_TIMEOUT) */
} D;

uint8_t *decoder_buffer(void)
{
    if (D.in_map)
        return D.in_map;
    uint64_t addr = 0;
    if (!D.in && jam_vmo_create(JAMCOVER_IN_BYTES, 0, HANDLE_INVALID, &D.in) != OK)
        return NULL;
    if (jam_vmar_map(startup_handle(SR_SELF_VMAR), D.in, 0, JAMCOVER_IN_BYTES,
                     VMAR_READ | VMAR_WRITE, &addr) != OK)
        return NULL;
    D.in_map = (uint8_t *)(uintptr_t)addr;
    return D.in_map;
}

static void helper_stop(void)
{
    if (D.job)
        jam_job_kill(D.job);
    handle_t *hs[] = { &D.ch, &D.proc, &D.job };
    for (unsigned i = 0; i < 3; i++)
        if (*hs[i]) {
            jam_handle_close(*hs[i]);
            *hs[i] = HANDLE_INVALID;
        }
}

/* The helper's handles (<jamcover.h>): its channel end and duplicates of
 * the two VMOs, into x; the rights its copies get into xr. */
static status_t helper_handles(struct spawn_handle *x, rights_t *xr)
{
    handle_t theirs, in, out;
    status_t st = jam_channel_create(&D.ch, &theirs);
    if (st != OK)
        return st;
    if ((st = jam_handle_duplicate(D.in, RIGHT_SAME, &in)) != OK) {
        jam_handle_close(theirs);
        return st;
    }
    if ((st = jam_handle_duplicate(D.out, RIGHT_SAME, &out)) != OK) {
        jam_handle_close(theirs);
        jam_handle_close(in);
        return st;
    }
    x[0] = (struct spawn_handle){ SR_USER + JAMCOVER_ROLE_CH, theirs };
    x[1] = (struct spawn_handle){ SR_USER + JAMCOVER_ROLE_IN, in };
    x[2] = (struct spawn_handle){ SR_USER + JAMCOVER_ROLE_OUT, out };
    xr[0] = RIGHT_READ | RIGHT_WRITE | RIGHT_WAIT;
    xr[1] = RIGHT_READ | RIGHT_MAP;
    xr[2] = RIGHT_READ | RIGHT_WRITE | RIGHT_MAP;
    return OK;
}

/* A new helper with nothing but its channel and the two VMOs. */
static status_t helper_start(void)
{
    if (!decoder_buffer())
        return ERR_NO_MEMORY;
    status_t st = D.out ? OK : jam_vmo_create(JAMCOVER_OUT_BYTES, 0, HANDLE_INVALID, &D.out);
    if (st == OK)
        st = jam_job_create(startup_handle(SR_JOB), 0, &D.job);
    struct spawn_handle x[3];
    rights_t xr[3];
    if (st == OK)
        st = helper_handles(x, xr);
    const char *argv[] = { JAMCOVER_PATH, D.test_arg, NULL };
    struct spawn_args a = {
        .path = JAMCOVER_PATH, .argc = D.test_arg ? 2 : 1, .argv = argv, .job = D.job,
        .extra = x, .nextra = 3, .extra_rights = xr,
    };
    if (st == OK)
        st = spawn(&a, &D.proc);   /* the extras go, whatever happens */
    if (st != OK) {
        helper_stop();
        return st;
    }
    D.starts++;
    return OK;
}

status_t decoder_decode(size_t len, bool large, uint32_t *small_px, uint32_t *large_px, int *w,
                        int *h)
{
    status_t st = D.ch ? OK : helper_start();
    uint32_t pw = 0, ph = 0;
    uint64_t wait = D.timeout ? D.timeout : JAMCOVER_TIMEOUT;
    if (st == OK)
        st = jamcover_decode_until(D.ch, now() + wait, len, large, &pw, &ph);
    if (st != OK && st != ERR_NOT_SUPPORTED && st != ERR_OUT_OF_RANGE && st != ERR_INVALID_ARGS) {
        helper_stop();   /* crashed, hung or confused: the next cover gets a new one */
        return st;
    }
    if (st == OK && (pw - 1 >= JAMCOVER_MAX_SIDE || ph - 1 >= JAMCOVER_MAX_SIDE)) {
        helper_stop();   /* a size it never decodes: it is not the helper we started */
        return ERR_INTERNAL;
    }
    if (st == OK)
        st = jam_vmo_read(D.out, JAMCOVER_SMALL_AT, small_px,
                          (uint64_t)JAMCOVER_SMALL * JAMCOVER_SMALL * 4);
    if (st == OK && large)
        st = jam_vmo_read(D.out, JAMCOVER_LARGE_AT, large_px,
                          (uint64_t)JAMCOVER_LARGE * JAMCOVER_LARGE * 4);
    if (st == OK) {
        *w = (int)pw;
        *h = (int)ph;
    }
    return st;
}

void decoder_test(const char *arg, uint64_t timeout)
{
    helper_stop();
    D.test_arg = arg;
    D.timeout = timeout;
}

unsigned decoder_test_starts(void)
{
    return D.starts;
}
