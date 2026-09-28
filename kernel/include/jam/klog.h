/* Kernel log: every line goes to the in-memory ring buffer, the framebuffer
 * console and (when present) the COM1 serial port. */
#pragma once

#include <stddef.h>

void   klog_write(const char *s, size_t len);
/* Copy up to `size` bytes of the most recent log text into buf. */
size_t klog_tail(char *buf, size_t size);
/* Write text that already carries its own timestamps (the panic screen's
 * copy of the log tail). */
void   klog_write_raw(const char *s, size_t len);
/* Panic only: drop the log lock in case this CPU died holding it. */
void   klog_force_unlock(void);
