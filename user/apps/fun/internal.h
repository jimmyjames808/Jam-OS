/* libfun: what its own files share and the apps don't see (fun.h is the
 * library's public side). */
#pragma once

#include <font.h>
#include "fun.h"

/* A present goes by bands of PBAND rows, each row in pieces of PSEG
 * pixels: a piece that differs from what is shown is written. */
#define PBAND 16
#define PSEG  64

/* gfx.c: present rows y0 .. y1 - 1 only (what changed in them). */
void gfx_present_rows(int y0, int y1);
/* gfx.c: n pixels from s to d (a plain loop of wide stores, never rep
 * movsb: write-combining memory likes it best, and QEMU too). */
void px_copy(uint32_t *restrict d, const uint32_t *restrict s, int n);
/* Whether n pixels are the same. */
static inline bool px_same(const uint32_t *a, const uint32_t *b, int n)
{
    const uint64_t *x = (const uint64_t *)a, *y = (const uint64_t *)b;
    uint64_t diff = 0;
    for (int i = 0; i < n / 2; i++)
        diff |= x[i] ^ y[i];
    if (n & 1)
        diff |= a[n - 1] ^ b[n - 1];
    return !diff;
}

/* mouse.c: one mouse report from the console into the pointer and the
 * buttons. True if it is one the app must see by itself (a button or the
 * wheel changed), false for plain movement, which may be merged. */
bool mouse_report(const struct input_mouse_event *ev);
/* mouse.c: draw the arrow into the back buffer for a present, and take it
 * out again after (the app's frame never keeps it). */
void pointer_paint(void);
void pointer_unpaint(void);
/* mouse.c: gfx_close's part: no mouse, no arrow. */
void mouse_close(void);
