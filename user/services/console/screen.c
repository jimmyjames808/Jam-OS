/* console: what the screen shows (console.h): the cells of the text
 * screen or the alternate screen, walked for winpaint.c, which draws them
 * into the window's buffers (at most every RENDER_NS). The compositor owns
 * the screen: there is no framebuffer here (M12's Q14). */
#include "console.h"

void grid_walk(void (*show)(uint32_t x, uint32_t y, struct cell c, uint8_t inv))
{
    if (alt_on) {
        for (uint32_t y = 0; y < rows; y++)
            for (uint32_t x = 0; x < cols; x++)
                show(x, y, alt[y * cols + x], alt_cursor && x == alt_x && y == alt_y);
        return;
    }
    struct cell empty = { ' ', A_OUT, 0 };
    clip_begin();   /* the selection's range, once for the walk */
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
            show(x, y, l ? l[x] : empty,
                 (uint8_t)((cursor_row && x == cur_x ? CELL_CURSOR : 0) |
                           (clip_marked(i, x) ? CELL_SELECTED : 0)));
    }
}

void render(void)
{
    if (alt_on && alt_sync && now() - alt_sync_since < 250000000ull)
        return;   /* mid-frame: stay dirty, draw once the frame is complete */
    if (!window_mode) {
        dirty = false;   /* no screen: COM1 has the text */
        return;
    }
    window_render();   /* stays dirty while both of its buffers are busy */
}
