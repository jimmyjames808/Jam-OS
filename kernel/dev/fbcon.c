/* Framebuffer text console.
 *
 * Text is kept in a cell grid so scrolling only ever WRITES to the
 * framebuffer: reading back write-combined video memory is very slow on real
 * hardware. When the cursor runs off the bottom, the console scrolls a third
 * of the screen at once so heavy logging does not redraw on every line.
 *
 * A process (the console) can take the screen (fbcon_take, through the
 * framebuffer_take system call). While it is taken the cell grid is still
 * kept up to date but nothing is drawn; fbcon_release redraws the grid, so
 * the screen shows the latest log again. A panic (fbcon_force_unlock)
 * takes the screen back for good.
 *
 * Quiet (a plain boot that shows the boot splash, fbcon_init): the screen
 * is filled with the splash's dark background at once and no text is
 * drawn on it, from the first kernel line until the console takes the
 * screen; the cells are kept as usual, so a panic or a released screen
 * shows the whole log. */
#include <stdbool.h>
#include <stdint.h>
#include <jam/fbcon.h>
#include <jam/spinlock.h>
#include <jam/status.h>

#define GLYPH_W  8
#define GLYPH_H  16
#define MAX_COLS 480   /* 3840 px */
#define MAX_ROWS 135   /* 2160 px */
#define TAB      4

extern const uint8_t font_8x16[128][16];

struct cell {
    char     ch;       /* the character in this cell */
    uint32_t fg, bg;   /* framebuffer-native pixel values */
};

static struct boot_framebuffer fb;
static bool ready;
static uint32_t cols, rows, cx, cy;
static uint32_t cur_fg, cur_bg;
static struct cell cells[MAX_ROWS][MAX_COLS];
static spinlock_t lock = SPINLOCK_INIT("fbcon");
/* A process owns the screen (the console, through framebuffer_take): keep
 * the cells, draw nothing. */
static bool taken;   /* fbcon_take: a process owns the screen, draw nothing */
/* The boot splash is coming: draw nothing (until a release or a panic). */
static bool quiet;

static uint32_t native(uint32_t rgb)
{
    return ((rgb >> 16 & 0xff) << fb.red_shift) |
           ((rgb >> 8 & 0xff) << fb.green_shift) |
           ((rgb & 0xff) << fb.blue_shift);
}

/* Drawing now: nobody owns the screen and it isn't the splash's. */
static bool drawing(void)
{
    return !__atomic_load_n(&taken, __ATOMIC_RELAXED) &&
           !__atomic_load_n(&quiet, __ATOMIC_RELAXED);
}

static void draw_cell(uint32_t col, uint32_t row)
{
    if (!drawing())
        return;
    const struct cell *c = &cells[row][col];
    const uint8_t *glyph = font_8x16[(uint8_t)c->ch & 0x7f];
    uint8_t *line = (uint8_t *)fb.virt + (uint64_t)row * GLYPH_H * fb.pitch + col * GLYPH_W * 4;

    for (int y = 0; y < GLYPH_H; y++, line += fb.pitch) {
        volatile uint32_t *px = (volatile uint32_t *)line;
        uint8_t bits = glyph[y];
        for (int x = 0; x < GLYPH_W; x++)
            px[x] = (bits & (0x80 >> x)) ? c->fg : c->bg;
    }
}

static void redraw_all(void)
{
    if (!drawing())
        return;
    for (uint32_t r = 0; r < rows; r++)
        for (uint32_t c = 0; c < cols; c++)
            draw_cell(c, r);
}

static void clear_row(uint32_t row)
{
    for (uint32_t c = 0; c < cols; c++)
        cells[row][c] = (struct cell){ ' ', cur_fg, cur_bg };
}

static void scroll(void)
{
    uint32_t n = rows / 3 ? rows / 3 : 1;
    for (uint32_t r = 0; r + n < rows; r++)
        for (uint32_t c = 0; c < cols; c++)
            cells[r][c] = cells[r + n][c];
    for (uint32_t r = rows - n; r < rows; r++)
        clear_row(r);
    cy = rows - n;
    redraw_all();
}

static void newline(void)
{
    cx = 0;
    if (++cy >= rows)
        scroll();
}

static void putc_locked(char ch)
{
    switch (ch) {
    case '\n':
        newline();
        return;
    case '\r':
        cx = 0;
        return;
    case '\t':
        do
            putc_locked(' ');
        while (cx % TAB);
        return;
    case '\b':
        if (cx)
            cx--;
        return;
    }
    cells[cy][cx] = (struct cell){ ch, cur_fg, cur_bg };
    draw_cell(cx, cy);
    if (++cx >= cols)
        newline();
}

/* The boot framebuffer's physical range, kept even when the console can't
 * drive it: the PCI core must never touch the device that holds it. */
static uint64_t fb_phys, fb_len;

uint64_t fbcon_phys(uint64_t *len)
{
    if (len)
        *len = fb_len;
    return fb_phys;
}

/* The whole framebuffer (the margins past the last cell too) in one colour. */
static void fill_screen(uint32_t rgb)
{
    uint32_t px = native(rgb);
    for (uint32_t y = 0; y < fb.height; y++) {
        volatile uint32_t *line =
            (volatile uint32_t *)((uint8_t *)fb.virt + (uint64_t)y * fb.pitch);
        for (uint32_t x = 0; x < fb.width; x++)
            line[x] = px;
    }
}

void fbcon_init(const struct boot_framebuffer *f, bool splash)
{
    if (f->virt) {
        fb_phys = f->phys;
        fb_len = (uint64_t)f->pitch * f->height;
    }
    if (!f->virt || f->bpp != 32)
        return;   /* only 32-bpp linear framebuffers are driven */
    fb = *f;
    cols = fb.width / GLYPH_W;
    rows = fb.height / GLYPH_H;
    if (cols > MAX_COLS)
        cols = MAX_COLS;
    if (rows > MAX_ROWS)
        rows = MAX_ROWS;
    cur_fg = native(0xd0d0d0);
    cur_bg = native(0x101018);
    ready = true;
    if (splash) {
        __atomic_store_n(&quiet, true, __ATOMIC_RELAXED);
        fill_screen(FBCON_SPLASH_BG);
    }
    fbcon_clear();
}

void fbcon_set_colors(uint32_t fg_rgb, uint32_t bg_rgb)
{
    if (!ready)
        return;
    cur_fg = native(fg_rgb);
    cur_bg = native(bg_rgb);
}

void fbcon_clear(void)
{
    if (!ready)
        return;
    uint64_t f = spin_lock_irqsave(&lock);
    for (uint32_t r = 0; r < rows; r++)
        clear_row(r);
    cx = cy = 0;
    redraw_all();
    spin_unlock_irqrestore(&lock, f);
}

void fbcon_write(const char *s, size_t len)
{
    if (!ready)
        return;
    uint64_t f = spin_lock_irqsave(&lock);
    for (size_t i = 0; i < len; i++)
        putc_locked(s[i]);
    spin_unlock_irqrestore(&lock, f);
}

uint64_t fbcon_time_redraw(uint64_t (*now)(void))
{
    if (!ready)
        return 0;
    uint64_t f = spin_lock_irqsave(&lock);
    uint64_t t0 = now();
    redraw_all();
    uint64_t t1 = now();
    spin_unlock_irqrestore(&lock, f);
    return t1 - t0;
}

void fbcon_force_unlock(void)
{
    spin_force_unlock(&lock);
    __atomic_store_n(&taken, false, __ATOMIC_RELAXED);   /* a panic always draws */
    __atomic_store_n(&quiet, false, __ATOMIC_RELAXED);
}

void fbcon_go_dark(void)
{
    spin_force_unlock(&lock);
    __atomic_store_n(&taken, false, __ATOMIC_RELAXED);
    __atomic_store_n(&quiet, true, __ATOMIC_RELAXED);
}

void fbcon_fill_splash_bg(void)
{
    if (ready)
        fill_screen(FBCON_SPLASH_BG);
}

bool fbcon_geometry(struct boot_framebuffer *out)
{
    if (!ready)
        return false;
    *out = fb;
    return true;
}

status_t fbcon_take(void)
{
    if (!ready)
        return ERR_NOT_FOUND;
    uint64_t f = spin_lock_irqsave(&lock);
    status_t st = __atomic_load_n(&taken, __ATOMIC_RELAXED) ? ERR_BAD_STATE : OK;
    __atomic_store_n(&taken, true, __ATOMIC_RELAXED);
    spin_unlock_irqrestore(&lock, f);
    return st;
}

void fbcon_release(void)
{
    if (!ready)
        return;
    uint64_t f = spin_lock_irqsave(&lock);
    __atomic_store_n(&taken, false, __ATOMIC_RELAXED);
    __atomic_store_n(&quiet, false, __ATOMIC_RELAXED);   /* the console is gone: show the log */
    redraw_all();
    spin_unlock_irqrestore(&lock, f);
}

void fbcon_unquiet(void)
{
    if (!ready || !__atomic_load_n(&quiet, __ATOMIC_RELAXED))
        return;
    uint64_t f = spin_lock_irqsave(&lock);
    __atomic_store_n(&quiet, false, __ATOMIC_RELAXED);
    redraw_all();   /* nothing if a process owns the screen: its release redraws */
    spin_unlock_irqrestore(&lock, f);
}

bool fbcon_is_taken(void)
{
    return __atomic_load_n(&taken, __ATOMIC_RELAXED);
}
