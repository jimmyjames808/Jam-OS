/* libfun: what its own files share and the apps don't see (fun.h is the
 * library's public side). */
#pragma once

#include "fun.h"

/* gfx.c: present rows y0 .. y1 - 1 only (what changed in them). */
void gfx_present_rows(int y0, int y1);

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
