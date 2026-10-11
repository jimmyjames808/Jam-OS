/* What a box (a multi-call program of Jam OS's own commands, sysbox.mk)
 * needs besides the shell's command code: the main that mkbox.sh writes
 * calls box_init before the command, and box_io.c is the other half of
 * the shell's output, input and handle calls (user/services/shell/sh.h)
 * for a POSIX program. */
#pragma once

/* Before the command `cmd`: Ctrl+C (SIGINT) is noted for sh_interrupted
 * instead of ending the program, and the shell's current directory is the
 * program's. needs_root: the command uses the root resource; if the
 * program has none (a POSIX parent started it) say so and return nonzero:
 * the command doesn't run. */
int box_init(const char *cmd, int needs_root);
