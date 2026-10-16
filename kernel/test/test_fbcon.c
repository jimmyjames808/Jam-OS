/* The kernel console after a lock break. reboot (quiet_machine) and kexec
 * halt the other CPUs by NMI, which stops one even in the middle of
 * fbcon_write, then break the console's lock and draw. A CPU halted
 * between newline()'s ++cy and the scroll leaves the cursor one row below
 * the screen: the next character went one row past the framebuffer's end
 * (a-ssh 2026-10-16: JAM-PF-8000 in draw_cell, reboot's first line, the
 * write at exactly fb + height * pitch). Each halted state here is put
 * under the lock the ordinary way (fbcon_test_break_at), so the test is
 * safe on a kernel whose other CPUs print. */
#include <jam/fbcon.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>

KTEST(fbcon_lock_break_cursor)
{
    struct boot_framebuffer fb;
    if (!fbcon_geometry(&fb)) {
        kprintf("fbcon_lock_break_cursor: no framebuffer: skipped\n");
        return;
    }
    KT_SKIP_LIVE("the console owns the screen");
    uint32_t x = 0, y = 0, cols, rows;
    fbcon_test_break_at(&x, &y, &cols, &rows);   /* the grid's size */
    KT_ASSERT(cols > 1 && rows > 1);

    /* Halted in newline() before (or during) the scroll: one row below. */
    x = 0;
    y = rows;
    fbcon_test_break_at(&x, &y, &cols, &rows);
    KT_ASSERT(x < cols);
    KT_ASSERT(y < rows);
    kprintf("fbcon_lock_break_cursor: after a break below the screen, drawn on row %u of %u\n",
            y, rows);

    /* Halted in glyph_locked() past the last column, before newline(). */
    x = cols;
    y = rows - 1;
    fbcon_test_break_at(&x, &y, &cols, &rows);
    KT_ASSERT(x < cols);
    KT_ASSERT(y < rows);
    kprintf("fbcon_lock_break_cursor: after a break past the last column, drawn on row %u\n", y);

    /* Past the last column of a row that isn't the last: the next row. */
    x = cols;
    y = 0;
    fbcon_test_break_at(&x, &y, &cols, &rows);
    KT_EQ(x, 0u);
    KT_EQ(y, 1u);

    /* A cursor already on the screen stays where it is. */
    x = 3;
    y = rows / 2;
    fbcon_test_break_at(&x, &y, &cols, &rows);
    KT_EQ(x, 3u);
    KT_EQ(y, rows / 2);

    /* Whatever the cursor says, nothing is drawn outside the grid: before
     * the fix the first of these wrote one byte past the framebuffer. */
    fbcon_test_draw_below();
    kprintf("fbcon_lock_break_cursor: nothing drawn below or right of the grid\n");
}
