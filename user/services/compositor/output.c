/* The output (comp.h): the screen the compositor composes onto.
 *
 * Normally the boot framebuffer, taken for good with framebuffer_take (the
 * root resource's RIGHT_ROOT_SCREEN): a write-combining VMO we map and only
 * ever write (reads of write-combining memory are very slow: nothing here
 * or in paint.c reads it), and an owner token we hold until we die. While
 * we hold it the kernel doesn't draw its log on the screen; when we die
 * the kernel draws it again, and a panic always draws over us.
 *
 * Without a framebuffer (the `headless` argument, no root resource, or
 * framebuffer_take's ERR_NOT_FOUND: a machine with no GOP framebuffer) the
 * output is an image in memory, 0x00RRGGBB, so the protocol and the
 * painting still work and tests can look at the result: the starter's VMO
 * (SR_USER + 1) or one of our own.
 *
 * Rows are written with 16-byte stores, which the PC's framebuffer takes
 * at its full 10.8 GB/s from one or two threads (4-byte stores reach half
 * that on one thread: fbbench, 2026-10-05). A framebuffer whose channels
 * sit elsewhere than 0x00RRGGBB gets each pixel converted. */
#include "paint.h"

#define SIDE_MAX 8192   /* a framebuffer's width or height, at most */

struct comp_output output;

typedef uint32_t v4u32 __attribute__((vector_size(16)));

/* The framebuffer, taken and mapped: OK, or why not (ERR_NOT_FOUND: there
 * is none). */
static status_t take_screen(handle_t root)
{
    struct fb_info fi;
    handle_t vmo;
    status_t st = jam_framebuffer_take(root, &fi, &vmo, &output.owner);
    if (st != OK)
        return st;
    uint64_t addr = 0, len = (fi.size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (fi.bpp != 32 || fi.pitch % 4 || fi.width < 1 || fi.height < 1 || fi.width > SIDE_MAX ||
        fi.height > SIDE_MAX || fi.pitch / 4 < fi.width || fi.size < (uint64_t)fi.pitch * fi.height)
        st = ERR_NOT_SUPPORTED;
    else
        st = jam_vmar_map(comp.vmar, vmo, 0, len, VMAR_READ | VMAR_WRITE, &addr);
    jam_handle_close(vmo);   /* the mapping keeps it */
    if (st != OK) {
        printf("compositor: the framebuffer (%ux%u, %u bits, pitch %u) is no use: %s\n", fi.width,
               fi.height, fi.bpp, fi.pitch, status_str(st));
        return st;   /* the owner token goes with us: the kernel draws again */
    }
    output.px = (uint32_t *)(uintptr_t)addr;
    output.stride = fi.pitch / 4;
    output.width = (int32_t)fi.width;
    output.height = (int32_t)fi.height;
    output.rs = fi.red_shift;
    output.gs = fi.green_shift;
    output.bs = fi.blue_shift;
    output.screen = true;
    return OK;
}

/* The headless image: the starter's VMO (SR_USER + 1) or one of our own,
 * mapped read-write. */
static status_t headless_image(int32_t w, int32_t h)
{
    uint64_t bytes = (uint64_t)w * (uint64_t)h * 4, size = 0, addr = 0;
    uint64_t mapped = (bytes + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    handle_t vmo = startup_handle(SR_USER + 1);
    status_t st = OK;
    if (vmo == HANDLE_INVALID)
        st = jam_vmo_create(mapped, 0, HANDLE_INVALID, &vmo);
    if (st == OK)
        st = jam_vmo_get_size(vmo, &size);
    if (st == OK && size < mapped)
        st = ERR_OUT_OF_RANGE;
    if (st == OK)
        st = jam_vmar_map(comp.vmar, vmo, 0, mapped, VMAR_READ | VMAR_WRITE, &addr);
    if (vmo != HANDLE_INVALID)
        jam_handle_close(vmo);   /* the mapping keeps it */
    if (st != OK)
        return st;
    output.px = (uint32_t *)(uintptr_t)addr;
    output.stride = (uint32_t)w;
    output.width = w;
    output.height = h;
    output.rs = 16;
    output.gs = 8;
    output.bs = 0;
    output.screen = false;
    return OK;
}

status_t output_open(bool headless, int32_t w, int32_t h)
{
    handle_t root = startup_handle(SR_RESOURCE);
    status_t st = ERR_NOT_FOUND;
    if (!headless && root != HANDLE_INVALID)
        st = take_screen(root);
    if (st == ERR_NOT_FOUND)
        st = headless_image(w, h);
    if (st != OK)
        return st;
    output.native = output.rs == 16 && output.gs == 8 && output.bs == 0;
    if (!output.screen) {
        scene.pixels = output.px;
        scene.stride = output.stride;
    }
    return OK;
}

/* n pixels from src to dst, top bytes cleared, 16 bytes a store. Not
 * turned into a call to memcpy (the attribute). */
__attribute__((optimize("no-tree-loop-distribute-patterns")))
static void put_native(uint32_t *restrict dst, const uint32_t *restrict src, int n)
{
    const v4u32 rgb = { 0xffffff, 0xffffff, 0xffffff, 0xffffff };
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        v4u32 v;
        __builtin_memcpy(&v, src + i, 16);
        v &= rgb;
        __builtin_memcpy(dst + i, &v, 16);
    }
    for (; i < n; i++)
        dst[i] = src[i] & 0xffffff;
}

/* The same into a framebuffer whose channels sit elsewhere. */
static void put_convert(uint32_t *restrict dst, const uint32_t *restrict src, int n)
{
    for (int i = 0; i < n; i++) {
        uint32_t c = src[i];
        dst[i] = (c >> 16 & 0xff) << output.rs | (c >> 8 & 0xff) << output.gs |
                 (c & 0xff) << output.bs;
    }
}

void output_put(int32_t x, int32_t y, const uint32_t *src, int n)
{
    uint32_t *dst = output.px + (uint64_t)y * output.stride + (uint32_t)x;
    if (output.native)
        put_native(dst, src, n);
    else
        put_convert(dst, src, n);
}
