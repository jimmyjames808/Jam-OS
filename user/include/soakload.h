/* What bin/soakload answers on its control channel when it is stopped
 * (user/tests/soakload/main.c): the shell's `soak` command reads it. */
#pragma once

#include <stdint.h>

struct soakload_result {
    uint64_t file_cycles;     /* files written, read back, compared and deleted */
    uint64_t read_passes;     /* passes over a read-only mount's files */
    uint64_t cut_short;       /* file cycles a failed call ended (a pulled stick) */
    uint64_t memory_rounds;   /* VMOs mapped, written, checked, unmapped */
    uint64_t calls;           /* channel calls answered and checked */
    uint64_t spawns;          /* programs started and their exit codes checked */
    uint64_t failed;          /* checks that failed, of every kind */
};
