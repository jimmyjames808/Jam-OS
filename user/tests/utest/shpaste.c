/* utest: the shell's side of a bracketed paste
 * (user/services/shell/sh_paste.c, linked in).
 *
 * t_sh_paste: the markers (ESC [ 2 0 0 ~ and ESC [ 2 0 1 ~, from a
 * keyboard's Escape key or a terminal's byte), a marker's start, near
 * misses, and what a pasted key puts on the line. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include "sh_paste.h"
#include "utest.h"

static struct input_key_event pk(uint16_t usage, uint32_t cp)
{
    return (struct input_key_event){ usage, INPUT_KEY_DOWN, 0, cp };
}

bool t_sh_paste(void)
{
    struct input_key_event k[6] = { pk(0x29, 0x1b), pk(0, '['), pk(0, '2'), pk(0, '0'),
                                    pk(0, '0'), pk(0, '~') };
    CHECK_EQ(sh_paste_marker(k, 1), SH_PASTE_MORE);
    CHECK_EQ(sh_paste_marker(k, 5), SH_PASTE_MORE);
    CHECK_EQ(sh_paste_marker(k, 6), SH_PASTE_BEGIN);
    k[0] = pk(0, 0x1b);   /* a terminal's ESC byte */
    CHECK_EQ(sh_paste_marker(k, 6), SH_PASTE_BEGIN);
    k[4] = pk(0x1e, '1');   /* a keyboard's '1' */
    CHECK_EQ(sh_paste_marker(k, 6), SH_PASTE_END);
    k[3] = pk(0, '1');
    CHECK_EQ(sh_paste_marker(k, 4), SH_PASTE_NO);   /* ESC [ 2 1 */
    k[0] = pk(0x04, 'a');
    CHECK_EQ(sh_paste_marker(k, 1), SH_PASTE_NO);
    CHECK(sh_paste_is_esc(&(struct input_key_event){ 0x29, INPUT_KEY_DOWN, 0, 0x1b }));
    CHECK(!sh_paste_is_esc(&k[0]));
    CHECK_EQ(sh_paste_byte(&k[0]), 'a');
    struct input_key_event x = pk(0, '\n');
    CHECK_EQ(sh_paste_byte(&x), ' ');
    x = pk(0x28, '\n');   /* Enter: a space, never a run */
    CHECK_EQ(sh_paste_byte(&x), ' ');
    x = pk(0, '\t');
    CHECK_EQ(sh_paste_byte(&x), ' ');
    x = pk(0, 0xe9);   /* the line holds ASCII */
    CHECK_EQ(sh_paste_byte(&x), 0);
    x = pk(0, 3);
    CHECK_EQ(sh_paste_byte(&x), 0);
    return true;
}
