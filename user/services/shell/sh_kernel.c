/* The kernel's side of the shell: its debug commands (ktest, bench,
 * stress, ps -k, kill, mem, pci, memmap, crash, panic; RIGHT_ROOT_DEBUG on
 * the root resource), which print into the kernel log (on the screen while
 * one runs, sh_show_log), and reading that log (RIGHT_ROOT_KLOG). */
#include "sh.h"

#define KLOG_STEP (64u << 10)   /* the buffer grows by doubling from this */
#define KLOG_MAX  (8u << 20)    /* and stops here: the kernel's ring (4 MiB) and
                                   what is logged while it is read */

int64_t sh_kcmd(const char *cmd)
{
    sh_flush();
    sh_show_log(true, NULL);   /* its output is the log */
    int64_t r = jam_debug_command(sh_root(), cmd, strlen(cmd));
    sh_show_log(false, NULL);
    if (r < 0)
        sh_say("%s: %s\n", cmd, status_str((status_t)r));
    return r;
}

/* A full buf moved into one twice its size, or NULL (buf freed) out of
 * memory. */
static char *grow(char *buf, size_t *size)
{
    char *more = malloc(*size * 2);
    if (more) {
        memcpy(more, buf, *size);
        *size *= 2;
    }
    free(buf);
    return more;
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
    size_t size = KLOG_STEP;
    char *buf = malloc(size);
    uint64_t pos = from, first = 0;
    int64_t n;
    while (buf && (n = jam_klog_read(r, pos, buf + *got, size - *got, &first)) > 0) {
        if (*got == 0)
            *start = first;
        *got += (size_t)n;
        pos = first + (uint64_t)n;
        if (*got < size)
            continue;
        if (size >= KLOG_MAX)
            break;   /* what was read is the answer */
        buf = grow(buf, &size);
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
