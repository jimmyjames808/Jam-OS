/* System calls 140-141: kexec (<jam/kexec.h>). Both need RIGHT_MANAGE on
 * a RES_ROOT handle, the right reboot needs: starting another kernel is
 * at least that much authority. kexec_load also needs RIGHT_READ on its two
 * VMOs, which it reads whole (the kernel maps them read-only while it
 * copies). The rules every sysc_* follows are in sysc.h. */
#include <jam/kexec.h>
#include <jam/kexec_handoff.h>
#include <jam/kprintf.h>
#include <jam/process.h>
#include <jam/syscall_impl.h>
#include <jam/vmo.h>
#include "sysc.h"

/* cmd[0..len) is a command line: printable ASCII, no NUL inside. */
static bool printable(const char *cmd, uint64_t len)
{
    for (uint64_t i = 0; i < len; i++)
        if (cmd[i] < 0x20 || cmd[i] > 0x7e)
            return false;
    return true;
}

int64_t sysc_kexec_load(handle_t root, handle_t kernel, handle_t bootfs, uint64_t ucmd,
                        uint64_t len, uint32_t flags)
{
    SYSC_TABLE(t);
    status_t st = sysc_get_root(t, root, RIGHT_MANAGE);
    if (st != OK)
        return st;
    char cmd[KEXEC_CMDLINE];
    if (flags || len >= sizeof(cmd) || copy_in(cmd, ucmd, len) != OK || !printable(cmd, len))
        return ERR_INVALID_ARGS;
    cmd[len] = '\0';
    struct kobject *k, *b;
    st = handle_get(t, kernel, OBJ_VMO, RIGHT_READ, &k, NULL);
    if (st != OK)
        return st;
    st = handle_get(t, bootfs, OBJ_VMO, RIGHT_READ, &b, NULL);
    if (st != OK) {
        kobject_unref(k);
        return st;
    }
    st = kexec_load_image(vmo_from_kobject(k), vmo_from_kobject(b), cmd);
    kobject_unref(b);
    kobject_unref(k);
    kprintf("kexec: kexec_load from %s: %s\n", process_name(process_current()), status_str(st));
    return st;
}

int64_t sysc_kexec_reboot(handle_t root)
{
    SYSC_TABLE(t);
    status_t st = sysc_get_root(t, root, RIGHT_MANAGE);
    if (st != OK)
        return st;
    kprintf("kexec: kexec_reboot from %s\n", process_name(process_current()));
    return kexec_reboot();
}
