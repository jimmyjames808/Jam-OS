/* The kernel's side of the shell: its debug commands (ktest, bench,
 * stress, ps -k, kill, mem, pci, memmap, crash, panic; RIGHT_MANAGE on the
 * root resource), which print into the kernel log, and reading that log
 * (RIGHT_READ). */
#include "sh.h"

#define KLOG_CAP (64 * 1024)

int64_t sh_kcmd(const char *cmd)
{
    sh_flush();
    int64_t r = jam_debug_command(sh_root(), cmd, strlen(cmd));
    if (r < 0)
        sh_say("%s: %s\n", cmd, status_str((status_t)r));
    return r;
}

status_t sh_klog_read(uint64_t from, char **out, size_t *got, uint64_t *start)
{
    handle_t r;
    *out = NULL;
    *got = 0;
    *start = 0;
    status_t st = jam_klog_open(sh_root(), &r);
    if (st != OK)
        return st;
    char *buf = malloc(KLOG_CAP);
    uint64_t pos = from, first = 0;
    int64_t n;
    while (buf && *got < KLOG_CAP &&
           (n = jam_klog_read(r, pos, buf + *got, KLOG_CAP - *got, &first)) > 0) {
        if (*got == 0)
            *start = first;
        *got += (size_t)n;
        pos = first + (uint64_t)n;
    }
    jam_handle_close(r);
    *out = buf;
    return buf ? OK : ERR_NO_MEMORY;
}

uint64_t sh_klog_end(void)
{
    handle_t r;
    uint64_t first = 0;
    char c;
    if (jam_klog_open(sh_root(), &r) != OK)
        return 0;
    jam_klog_read(r, UINT64_MAX, &c, 1, &first);   /* past the end: 0 bytes, first = the end */
    jam_handle_close(r);
    return first;
}
