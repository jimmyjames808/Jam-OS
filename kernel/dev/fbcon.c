/* Framebuffer text console.
 *
 * Text is kept in a cell grid so scrolling only ever WRITES to the
 * framebuffer: reading back write-combined video memory is very slow on real
 * hardware. When the cursor runs off the bottom, the console scrolls a third
 * of the screen at once so heavy logging does not redraw on every line. */
#include <stdbool.h>
#include <stdint.h>
#include <jam/fbcon.h>
#include <jam/spinlock.h>

#define GLYPH_W  8
#define GLYPH_H  16
#define MAX_COLS 480   /* 3840 px */
#define MAX_ROWS 135   /* 2160 px */
#define TAB      4

extern const uint8_t font_8x16[128][16];

struct cell {
    char     ch;
    uint32_t fg, bg;   /* framebuffer-native pixel values */
};

static struct boot_framebuffer fb;
static bool ready;
static uint32_t cols, rows, cx, cy;
static uint32_t cur_fg, cur_bg;
static struct cell cells[MAX_ROWS][MAX_COLS];
static spinlock_t lock = SPINLOCK_INIT;

static uint32_t native(uint32_t rgb)
{
    return ((rgb >> 16 & 0xff) << fb.red_shift) |
           ((rgb >> 8 & 0xff) << fb.green_shift) |
           ((rgb & 0xff) << fb.blue_shift);
}

static void draw_cell(uint32_t col, uint32_t row)
{
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

void fbcon_init(const struct boot_framebuffer *f)
{
    if (!f->virt || f->bpp != 32)
        return;   /* M0 only drives 32-bpp linear framebuffers */
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
    spin_lock(&lock);
    for (uint32_t r = 0; r < rows; r++)
        clear_row(r);
    cx = cy = 0;
    redraw_all();
    spin_unlock(&lock);
}

void fbcon_write(const char *s, size_t len)
{
    if (!ready)
        return;
    spin_lock(&lock);
    for (size_t i = 0; i < len; i++)
        putc_locked(s[i]);
    spin_unlock(&lock);
}

uint64_t fbcon_time_redraw(uint64_t (*now)(void))
{
    if (!ready)
        return 0;
    spin_lock(&lock);
    uint64_t t0 = now();
    redraw_all();
    uint64_t t1 = now();
    spin_unlock(&lock);
    return t1 - t0;
}

void fbcon_force_unlock(void)
{
    spin_unlock(&lock);
}
