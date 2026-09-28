#pragma once

#include <stddef.h>
#include <stdint.h>
#include <jam/boot.h>

void fbcon_init(const struct boot_framebuffer *fb);
void fbcon_write(const char *s, size_t len);
void fbcon_set_colors(uint32_t fg_rgb, uint32_t bg_rgb);
void fbcon_clear(void);
/* Drop any console lock state so panic output always gets through. */
void fbcon_force_unlock(void);
