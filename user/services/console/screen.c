/* console: the framebuffer (console.h).
 *
 * Drawing only ever writes to the framebuffer (reading write-combining
 * memory back is very slow): a shadow grid remembers what each screen cell
 * shows, and a render pass (at most every RENDER_NS) draws the cells that
 * changed.
 *
 * console.lend_screen lends the framebuffer to a program that draws on it
 * itself (bin/jamjar, bin/splash, the test programs fractal and wltest,
 * through libfun's gfx_open, under `nocomp`): it gets the VMO (without RIGHT_DUPLICATE) and the
 * geometry, and a lease channel. Meanwhile nothing is drawn (the text
 * model, the kernel log and the serial mirror keep going); when the
 * lease's other end closes (the program closed it or died), the whole
 * screen is redrawn.
 *
 * Quiet (the boot splash, screen_quiet): nothing is drawn at all until the
 * first lease ends, so the screen goes straight from the kernel's dark
 * background to the splash and back to the text.
 *
 * Blank (console.blank, for a reboot by kexec): the whole framebuffer the
 * splash's background and nothing drawn, until blank is turned off, so
 * the screen goes straight from the shell to the next boot's splash.
 *
 * In window mode (window.c) there is no framebuffer here: render() hands
 * over to winpaint.c, nothing is lent (ERR_NOT_SUPPORTED) and blank does
 * nothing (the compositor owns the screen). */
#include <splash.h>
#include "console.h"

static volatile uint32_t *fbp;
static struct fb_info fbi;
static handle_t fb_vmo;            /* kept for lend_screen */
static handle_t lease;             /* while the screen is lent: our end */
static uint32_t native[16];        /* the palette in the framebuffer's format */
static struct cell *shadow;        /* rows * cols: what each screen cell shows */
static uint8_t *shadow_cursor;     /* rows * cols: drawn inverted */
static uint64_t quiet_until;       /* uptime ns: draw nothing before it (0: not quiet) */
static bool blanked;               /* console.blank: draw nothing at all */

static void draw_cell(uint32_t x, uint32_t y, struct cell c, bool inverse)
{
    uint32_t fg = native[c.attr & 15], bg = native[c.attr >> 4];
    if (inverse) {
        uint32_t t = fg;
        fg = bg;
        bg = t;
    }
    uint8_t block[GH];
    const uint8_t *g = cell_bits(c, block);
    volatile uint32_t *row = fbp + (uint64_t)y * GH * (fbi.pitch / 4) + x * GW;
    for (int i = 0; i < GH; i++, row += fbi.pitch / 4) {
        uint8_t bits = g[i];
        for (int j = 0; j < GW; j++)
            row[j] = (bits & (0x80 >> j)) ? fg : bg;
    }
}

/* Cell (x, y) should show c (inverted: the cursor): drawn if it doesn't. */
static void show_cell(uint32_t x, uint32_t y, struct cell c, uint8_t inv)
{
    struct cell *s = &shadow[y * cols + x];
    if (!cell_same(*s, c) || shadow_cursor[y * cols + x] != inv) {
        draw_cell(x, y, c, inv);
        *s = c;
        shadow_cursor[y * cols + x] = inv;
    }
}

void screen_quiet(uint64_t until)
{
    quiet_until = until;
}

/* Every cell drawn again at the next render (the shadow grid forgets). */
static void forget_shadow(void)
{
    memset(shadow, 0, (size_t)rows * cols * sizeof(struct cell));
    memset(shadow_cursor, 0, (size_t)rows * cols);
    dirty = true;
}

void screen_blank(bool on)
{
    if (window_mode)
        return;   /* the compositor's screen: it blanks it */
    if (!on) {
        if (blanked && shadow)
            forget_shadow();
        blanked = false;
        return;
    }
    blanked = true;
    if (!fbp)
        return;
    uint32_t px = ((SPLASH_BG >> 16 & 0xff) << fbi.red_shift) |
                  ((SPLASH_BG >> 8 & 0xff) << fbi.green_shift) |
                  ((SPLASH_BG & 0xff) << fbi.blue_shift);
    for (uint32_t y = 0; y < fbi.height; y++)
        for (uint32_t x = 0; x < fbi.width; x++)
            fbp[(uint64_t)y * (fbi.pitch / 4) + x] = px;
}

void grid_walk(void (*show)(uint32_t x, uint32_t y, struct cell c, uint8_t inv))
{
    if (alt_on) {
        for (uint32_t y = 0; y < rows; y++)
            for (uint32_t x = 0; x < cols; x++)
                show(x, y, alt[y * cols + x], alt_cursor && x == alt_x && y == alt_y);
        return;
    }
    struct cell empty = { ' ', A_OUT, 0 };
    struct view v = view_now();
    int64_t first = view_base(&v) - view_back;   /* the line on row 0 (view.c) */
    int64_t oldest = committed > SCROLLBACK ? (int64_t)(committed - SCROLLBACK) : 0;
    for (uint32_t y = 0; y < rows; y++) {
        const struct cell *l = NULL;
        int64_t i = first + y;
        bool cursor_row = i == (int64_t)committed;
        if (cursor_row)
            l = cur;
        else if (i >= oldest && i < (int64_t)committed)
            l = line((uint64_t)i);
        for (uint32_t x = 0; x < cols; x++)
            show(x, y, l ? l[x] : empty, cursor_row && x == cur_x);
    }
}

void render(void)
{
    if (blanked) {
        dirty = false;   /* nothing to draw until blank is off, which redraws it all */
        return;
    }
    if (alt_on && alt_sync && now() - alt_sync_since < 250000000ull)
        return;   /* mid-frame: stay dirty, draw once the frame is complete */
    if (quiet_until && now() < quiet_until)
        return;   /* stay dirty: drawn once the splash is done */
    if (window_mode) {
        window_render();   /* stays dirty while both of its buffers are busy */
        return;
    }
    dirty = false;
    if (!fbp || lease)
        return;   /* no screen, or lent: drawn in full when it comes back */
    grid_walk(show_cell);
}

bool screen_init(void)
{
    handle_t vmo, owner;
    status_t st = jam_framebuffer_take(root, &fbi, &vmo, &owner);
    if (st != OK) {
        printf("console: no screen (%s): serial only\n", status_str(st));
        return false;
    }
    uint64_t addr = 0;
    st = jam_vmar_map(startup_handle(SR_SELF_VMAR), vmo, 0, fbi.size, VMAR_READ | VMAR_WRITE,
                      &addr);
    if (st != OK) {   /* else `vmo` is kept for lend_screen; `owner` stays open for good */
        printf("console: can't map the framebuffer (%s)\n", status_str(st));
        jam_handle_close(vmo);
        jam_handle_close(owner);
        return false;
    }
    fbp = (volatile uint32_t *)(uintptr_t)addr;
    fb_vmo = vmo;
    for (int i = 0; i < 16; i++) {
        uint32_t c = cell_palette[i];
        native[i] = ((c >> 16 & 0xff) << fbi.red_shift) | ((c >> 8 & 0xff) << fbi.green_shift) |
                    ((c & 0xff) << fbi.blue_shift);
    }
    cols = fbi.width / GW;
    rows = fbi.height / GH;
    if (cols > MAX_COLS)
        cols = MAX_COLS;
    if (rows > MAX_ROWS)
        rows = MAX_ROWS;
    return true;
}

bool screen_alloc(void)
{
    shadow = malloc((size_t)rows * cols * sizeof(struct cell));
    shadow_cursor = malloc((size_t)rows * cols);
    if (!shadow || !shadow_cursor)
        return false;
    memset(shadow, 0, (size_t)rows * cols * sizeof(struct cell));   /* ch 0: redraw all */
    memset(shadow_cursor, 0, (size_t)rows * cols);
    return true;
}

#define LENT_VMO_RIGHTS (RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_TRANSFER | RIGHT_WAIT | \
                         RIGHT_INSPECT)

status_t op_lend_screen(void *ctx, uint32_t *w, uint32_t *h, uint32_t *pitch, uint8_t *rs,
                        uint8_t *gs, uint8_t *bs, uint64_t *size, handle_t *screen,
                        handle_t *out_lease)
{
    (void)ctx;
    if (window_mode)
        return ERR_NOT_SUPPORTED;   /* the screen is the compositor's: a program opens a window */
    if (!fbp || !fb_vmo)
        return ERR_NOT_FOUND;
    if (lease)
        return ERR_BAD_STATE;
    handle_t v, mine, theirs;
    status_t st = jam_handle_duplicate(fb_vmo, LENT_VMO_RIGHTS, &v);
    if (st != OK)
        return st;
    if ((st = jam_channel_create(&mine, &theirs)) != OK) {
        jam_handle_close(v);
        return st;
    }
    if ((st = jam_port_bind(port, mine, KEY(K_LEASE, 0), SIG_PEER_CLOSED, PORT_BIND_ONCE)) != OK) {
        jam_handle_close(v);
        jam_handle_close(mine);
        jam_handle_close(theirs);
        return st;
    }
    lease = mine;
    *w = fbi.width;
    *h = fbi.height;
    *pitch = fbi.pitch;
    *rs = fbi.red_shift;
    *gs = fbi.green_shift;
    *bs = fbi.blue_shift;
    *size = fbi.size;
    *screen = v;
    *out_lease = theirs;
    printf("console: the screen is lent out\n");
    return OK;
}

bool screen_lent(void)
{
    return lease != HANDLE_INVALID;
}

/* The lease's other end closed: the screen is ours again, all of it
 * redrawn (the shadow grid forgets what it showed). */
void lease_ended(void)
{
    if (!lease)
        return;
    jam_handle_close(lease);
    lease = HANDLE_INVALID;
    quiet_until = 0;
    forget_shadow();
    printf("console: the screen is back\n");
}
