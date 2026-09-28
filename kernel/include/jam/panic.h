#pragma once

_Noreturn void panic(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
_Noreturn void halt_forever(void);

struct trap_frame;
/* Panic screen for a CPU exception: registers, decoded fault, backtrace. */
_Noreturn void panic_trap(const struct trap_frame *f);

#define ASSERT(cond) \
    do { if (!(cond)) panic("assertion failed: %s (%s:%d)", #cond, __FILE__, __LINE__); } while (0)
