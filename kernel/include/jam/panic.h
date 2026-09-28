#pragma once

_Noreturn void panic(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
_Noreturn void halt_forever(void);

#define ASSERT(cond) \
    do { if (!(cond)) panic("assertion failed: %s (%s:%d)", #cond, __FILE__, __LINE__); } while (0)
