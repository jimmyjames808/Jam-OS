/* init's side of the crash result channel (<crashlog.h>, the protocol
 * `crashres` in abi/idl/logctl.idl): the one `result` event logd sends,
 * read with the generated reader, which drops every message that breaks
 * the format. The values are made safe here: logd is another process, and
 * what it says goes into init's log and the shell's note. */
#include <crashlog.h>
#include <idl/logctl.h>
#include <os.h>

#define ROUND 8   /* messages read in one call */

_Static_assert(CRASHLOG_PATH_MAX == CRASHRES_PATH_MAX + 1, "crashres.result's path and <crashlog.h>'s");

static void on_result(void *ctx, int32_t status, const char *path, uint32_t path_len)
{
    struct crashlog_result *r = ctx;
    if (r->have)
        return;   /* one result is all there is: a second is no news */
    r->have = true;
    r->status = status > 0 ? ERR_INTERNAL : status;   /* an error code is 0 or below */
    uint32_t n = path_len < sizeof(r->path) - 1 ? path_len : (uint32_t)sizeof(r->path) - 1;
    for (uint32_t i = 0; i < n; i++)
        r->path[i] = path[i] >= ' ' && path[i] <= '~' ? path[i] : '?';
    r->path[n] = '\0';
}

status_t crashlog_read_result(handle_t ch, struct crashlog_result *r)
{
    static const struct crashres_ops ops = { .result = on_result };
    *r = (struct crashlog_result){ 0 };
    status_t st = OK;
    for (unsigned n = 0; st == OK && !r->have && n < ROUND; n++)
        st = crashres_serve_one(ch, &ops, r);
    return r->have ? OK : st == OK ? ERR_SHOULD_WAIT : st;
}
