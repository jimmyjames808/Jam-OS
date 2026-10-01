/* log: the last lines of the kernel log (default 20, at most 1000), grey. */
#include "sh.h"

#define TAIL (256u << 10)   /* bytes read from the log's end: 1000 lines and more */

SH_CMD(log)
{
    unsigned want = 20;
    if (argc > 1) {
        want = 0;
        for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++)
            want = want * 10 + (unsigned)(*p - '0');
        if (!want || want > 1000)
            want = 20;
    }
    char *buf;
    size_t got;
    uint64_t first;
    uint64_t end = sh_klog_end();
    status_t st = sh_klog_read(end > TAIL ? end - TAIL : 0, &buf, &got, &first);
    if (st == ERR_NO_MEMORY) {
        sh_say("log: out of memory\n");
        return 0;
    }
    if (st != OK) {
        sh_say("log: %s\n", status_str(st));
        return 0;
    }
    /* The last `want` complete lines. */
    size_t start = got;
    unsigned lines = 0;
    while (start > 0) {
        if (buf[start - 1] == '\n' && start != got && ++lines == want)
            break;
        start--;
    }
    if (start == 0 && first > 0) {   /* the read began mid-line: from the next one */
        while (start < got && buf[start] != '\n')
            start++;
        if (start < got)
            start++;
    }
    sh_say("\033[90m");
    sh_put(buf + start, got - start);
    sh_say("\033[0m");
    free(buf);
    return 0;
}
