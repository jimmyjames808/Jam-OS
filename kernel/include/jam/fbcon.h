#pragma once

#include <stddef.h>
#include <stdint.h>
#include <jam/boot.h>

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
/* Drop any console lock state so panic output always gets through. */
void fbcon_force_unlock(void);
