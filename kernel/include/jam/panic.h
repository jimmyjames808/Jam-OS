/* panic and ASSERT: for broken kernel invariants only, never for anything
 * user code can cause. A panic stops every CPU, prints the reason and a
 * backtrace on the serial port and the screen, and halts (debug/panic.c). */
#pragma once

#include <stdint.h>

_Noreturn void panic(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
_Noreturn void halt_forever(void);
/* The end of a panic screen (and of a crash kernel's): "system halted."
 * for 0 seconds, else a count down and a firmware reboot (panic_reboot). */
_Noreturn void panic_halt_or_reboot(uint64_t seconds);

/* One line every panic screen shows under its message: what the kernel was
 * in the middle of (the test runner's loop, seed and test). Set by one
 * writer at a time; "" clears it. */
void panic_note_set(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

struct trap_frame;
/* Panic screen for a CPU exception: registers, decoded fault, backtrace. */
_Noreturn void panic_trap(const struct trap_frame *f);
/* Watchdog NMI: this CPU was stuck; show where. */
_Noreturn void panic_watchdog(const struct trap_frame *f);

#define ASSERT(cond) \
    do { if (!(cond)) panic("assertion failed: %s (%s:%d)", #cond, __FILE__, __LINE__); } while (0)
