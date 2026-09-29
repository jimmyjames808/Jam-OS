/* utest internals: the suite (main.c), the child modes it spawns
 * (child.c) and the benchmark modes the kernel's bench entry spawns
 * (bench.c). */
#pragma once

#define CHANNEL_MSG_MAX 65536   /* CHANNEL_MAX_BYTES in the kernel */

/* "utest <mode> ...": run one child mode, return its exit code. */
int child_main(int argc, char **argv);
/* "utest bench-<what> ...": one user-side benchmark (see bench.c). */
int bench_child(int argc, char **argv);
