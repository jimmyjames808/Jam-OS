/* The processes whose kernel-log lines the console turns into notices:
 * init, devmgr and logd, by koid. The kernel marks every log line a
 * process writes with its koid (syscall klog_lines), so a line is
 * theirs only if its mark says so, whatever name or text it carries (any
 * program can start a process called "init"). init keeps this table in a
 * page of its own (user/services/init/writers.c), writes a koid there as
 * it starts each of them, and gives every console it starts the page
 * read-only (CONSOLE_WRITERS_ROLE); the console maps it and reads it at
 * each line (user/services/console/main.c). A field is 0 until its
 * process has started; a koid is never reused, so an old one stays
 * harmless after its process has gone. */
#pragma once

#include <stdint.h>
#include <os.h>

struct log_writers {
    uint64_t init;     /* init's koid */
    uint64_t devmgr;   /* the running (or last) devmgr's */
    uint64_t logd;     /* the running (or last) logd's */
};

/* The console's startup handle for the page: RIGHT_READ | RIGHT_MAP (and
 * RIGHT_TRANSFER, which spawn needs to hand it over); never RIGHT_WRITE. */
#define CONSOLE_WRITERS_ROLE (SR_USER + 9)
