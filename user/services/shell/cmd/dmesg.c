/* dmesg: the whole kernel log (what the kernel's ring still holds, up to
 * 4 MiB), passed on a piece at a time: the shell never holds more than
 * one piece of it. It stops at the end the log had when it started, so a
 * log that grows meanwhile can't keep it going. */
#include "sh.h"

#define PIECE (64u << 10)   /* bytes read from the kernel at a time */

SH_CMD(dmesg)
{
    (void)argc;
    (void)argv;
    handle_t r;
    status_t st = jam_klog_open(sh_root(), &r);
    char *buf = st == OK ? malloc(PIECE) : NULL;
    if (st == OK && !buf) {
        jam_handle_close(r);
        st = ERR_NO_MEMORY;
    }
    if (st != OK) {
        sh_tty("dmesg: %s\n", st == ERR_NO_MEMORY ? "out of memory" : status_str(st));
        return 1;
    }
    uint64_t end = 0, pos = 0, first = 0;
    jam_klog_read(r, UINT64_MAX, buf, 1, &end);   /* past the end: 0 bytes, the end */
    bool cut = true;   /* until a newline: the ring may have cut the first line */
    int64_t n;
    while (pos < end && !sh_interrupted() &&
           (n = jam_klog_read(r, pos, buf, PIECE, &first)) > 0) {
        if (first >= end)
            break;   /* all it had when it started has gone by meanwhile */
        size_t at = 0, len = (size_t)n;
        if (first == 0)
            cut = false;
        while (cut && at < len)
            cut = buf[at++] != '\n';
        if (first + len > end)
            len = (size_t)(end - first);   /* logged since dmesg started: left out */
        if (at < len)
            sh_put(buf + at, len - at);
        pos = first + (uint64_t)n;
    }
    free(buf);
    jam_handle_close(r);
    return 0;
}
