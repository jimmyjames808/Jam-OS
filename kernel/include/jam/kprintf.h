/* Formatted kernel output. kprintf writes to the kernel log (klog), which
 * feeds the serial port and the framebuffer console; ksnprintf formats into
 * a buffer and never writes past `size`. */
#pragma once

#include <stdarg.h>
#include <stddef.h>

/* Supported: %d %i %u %x %X %p %s %c %% with l/ll/z modifiers, '-' and '0'
 * flags and a field width (e.g. %016lx, %-14s). */
int  kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int  ksnprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
