/* The kernel's text console on the boot framebuffer (dev/fbcon.c). It
 * draws the kernel log until the console process takes the screen
 * (framebuffer_take), and again after a panic, which always draws. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <jam/boot.h>
#include <jam/status.h>

void fbcon_init(const struct boot_framebuffer *fb);
void fbcon_write(const char *s, size_t len);
void fbcon_set_colors(uint32_t fg_rgb, uint32_t bg_rgb);
void fbcon_clear(void);
/* Redraw the whole screen once, returning the elapsed time in the units
 * of now() (used to measure how fast the framebuffer really is). */
uint64_t fbcon_time_redraw(uint64_t (*now)(void));
/* Physical address (and length) of the boot framebuffer, 0 if there is
 * none; recorded even when the console can't draw on it. */
uint64_t fbcon_phys(uint64_t *len);
/* Drop any console lock state so panic output always gets through; also
 * takes the screen back from a process that owns it (a panic always draws). */
void fbcon_force_unlock(void);

/* Screen hand-off (framebuffer_take, kernel/abi/sysc_console.c).
 * fbcon_geometry: false if fbcon can't draw (no 32-bpp framebuffer).
 * fbcon_take: from now on nothing is drawn (the text is still kept);
 * ERR_BAD_STATE if already taken, ERR_NOT_FOUND without a framebuffer.
 * fbcon_release: draw again, starting with a redraw of the kept text. */
bool     fbcon_geometry(struct boot_framebuffer *out);
status_t fbcon_take(void);
void     fbcon_release(void);
bool     fbcon_is_taken(void);
