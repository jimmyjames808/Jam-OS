/* hda: the HD Audio controller's codecs and their widget graphs, as the
 * driver (drivers/hda) reads them now: the same lines it printed to the
 * log when it started (abi/idl/hda.idl). */
#include <devmgr.h>
#include <idl/hda.h>
#include "sh.h"

#define DUMP_WAIT (10 * NS_PER_S)   /* a codec's dump is a few hundred verbs */
#define DUMP_MAX  (64 * 1024)       /* the driver's dump buffer */

/* Ask each running PCI driver in turn for hda.dump; others answer
 * ERR_NOT_SUPPORTED. Prints each dump; returns how many answered. */
static unsigned dump_each(handle_t dm)
{
    unsigned found = 0;
    for (uint32_t n = 0; n < 32 && !sh_interrupted(); n++) {
        struct devmgr_rep r;
        handle_t ch, text;
        uint32_t nh = 0, len = 0, codecs = 0;
        status_t st = devmgr_call(dm, DEVMGR_GET_SERVICE, 0xffff, 0xffff, n, &r, &ch, 1, &nh,
                                  now() + 5 * NS_PER_S);
        if (st == ERR_NOT_FOUND)
            break;
        if (st != OK || nh != 1)
            continue;
        st = hda_dump_until(ch, now() + DUMP_WAIT, &text, &len, &codecs);
        jam_handle_close(ch);
        if (st == ERR_NOT_SUPPORTED)
            continue;   /* another driver's service */
        found++;
        if (st != OK) {
            sh_say("hda: %s\n", status_str(st));
            continue;
        }
        char *buf = len && len <= DUMP_MAX ? malloc(len) : NULL;
        if (buf && jam_vmo_read(text, 0, buf, len) == OK)
            sh_put(buf, len);
        else
            sh_say("hda: can't read the dump (%u bytes)\n", len);
        free(buf);
        jam_handle_close(text);
    }
    return found;
}

SH_CMD(hda)
{
    (void)argc;
    (void)argv;
    handle_t dm = sh_devmgr();
    if (!dm) {
        sh_say("hda: no devmgr\n");
        return 1;
    }
    if (!dump_each(dm)) {
        sh_say("hda: no HD Audio driver bound (devmgr has none that answers hda)\n");
        return 1;
    }
    return 0;
}
