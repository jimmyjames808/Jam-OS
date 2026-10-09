/* musl's signal restorer on Jam OS: overrides upstream's
 * src/signal/x86_64/restore.s (user/posix/musl/OVERRIDES), which ends a
 * Linux signal handler with rt_sigreturn. Jam OS's kernel never runs a
 * signal handler (signals are delivered by libc at the call boundary:
 * M13-PLAN section 6), so nothing returns here; if something ever does,
 * it traps. */

    .text
    .global __restore_rt
    .hidden __restore_rt
    .type __restore_rt, @function
__restore_rt:
    ud2
    .size __restore_rt, . - __restore_rt

    .section .note.GNU-stack, "", @progbits
