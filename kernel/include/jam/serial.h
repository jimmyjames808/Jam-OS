#pragma once

#include <stdbool.h>
#include <stddef.h>

/* COM1. Most modern PCs have no serial port; init detects that and the
 * write becomes a no-op. In QEMU it is the main log channel. */
bool serial_init(void);
void serial_write(const char *s, size_t len);
