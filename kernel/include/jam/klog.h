/* Kernel log: every line goes to the in-memory ring buffer, the framebuffer
 * console and (when present) the COM1 serial port. */
#pragma once

#include <stddef.h>

void   klog_write(const char *s, size_t len);
/* Copy up to `size` bytes of the most recent log text into buf. */
size_t klog_tail(char *buf, size_t size);
