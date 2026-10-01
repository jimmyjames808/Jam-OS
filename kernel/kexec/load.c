/* kexec's two loads: the crash kernel at boot, from the boot modules, and
 * a reboot image from two VMOs (the kexec_load system call).
 *
 * The crash kernel is this boot's own kernel and bootfs: jamos.elf loaded
 * a second time by Limine as a module (the running image's .data and
 * .bss have changed since it started, so it is no source), and the bootfs
 * module, which nothing ever writes. Its command line:
 *   crash                  the mode (main.c): save the log, nothing else
 *   crashlog=<phys>        the crash record (jump.c)
 *   crash_reboot=<s>       this kernel's panic_reboot: what the crash
 *                          kernel does once the log is saved. Never
 *                          panic_reboot itself, so a panic in the crash
 *                          kernel halts on its screen
 *   test<name>             from crashtest=<name>: a boot-time crash test
 *                          in the crash kernel (tools/kdump-test.sh)
 * and the switches that work around hardware (nopcid, ...), which it
 * needs as much as this kernel did. */
#include <stdarg.h>
#include <jam/bootfs.h>
#include <jam/cmdline.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/string.h>
#include <jam/vmo.h>
#include "kexec_internal.h"

#define CRASHTEST_MAX 16   /* a crash test's name */
#define KERNEL_FILE_MAX (64ull << 20)
#define BOOTFS_FILE_MAX (256ull << 20)

/* Boot words a crash kernel inherits: each turns off something the
 * hardware may not take (TESTING.md lists them). */
static const char *const inherited[] = {
    "nopcid", "forcepcid", "nodeadline", "noserialirq", "nooneshot", "nofpuopt",
    "nokmcache", "nospinidle", "noplaceorder", "noaffinepair",
};

static size_t append(char *buf, size_t size, size_t at, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static size_t append(char *buf, size_t size, size_t at, const char *fmt, ...)
{
    if (at >= size)
        return at;
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf + at, size - at, fmt, ap);
    va_end(ap);
    return n > 0 ? at + (size_t)n : at;
}

static void crash_cmdline(char *buf, size_t size)
{
    uint64_t reboot_s = cmdline_get_u64("panic_reboot", 0, 0);
    size_t at = append(buf, size, 0, "crash crashlog=%lu crash_reboot=%lu", kx_record_phys(),
                       reboot_s > 3600 ? 3600 : reboot_s);
    for (size_t i = 0; i < sizeof(inherited) / sizeof(inherited[0]); i++)
        if (cmdline_has(inherited[i]))
            at = append(buf, size, at, " %s", inherited[i]);
    char test[CRASHTEST_MAX];
    if (cmdline_get_str("crashtest", test, sizeof(test)))
        at = append(buf, size, at, " test%s", test);
}

/* The boot module whose path ends in `suffix`, or NULL. */
static const struct boot_module *module(const char *suffix)
{
    size_t sl = strlen(suffix);
    for (size_t i = 0; i < kx_boot->module_count; i++) {
        const struct boot_module *m = &kx_boot->modules[i];
        size_t pl = strlen(m->path);
        if (pl >= sl && !strcmp(m->path + pl - sl, suffix))
            return m;
    }
    return NULL;
}

/* Load under the lock; `done` is the state on success. A refused image
 * leaves the state as it was (kx_build writes nothing then): a crash
 * kernel stays armed. */
static status_t load(const struct kx_image *im, int done)
{
    mutex_lock(&kx_lock);
    int s = __atomic_load_n(&kx_state, __ATOMIC_ACQUIRE);
    if (s == KX_OFF || s == KX_JUMPING) {
        mutex_unlock(&kx_lock);
        return s == KX_OFF ? ERR_NOT_SUPPORTED : ERR_BAD_STATE;
    }
    status_t st = kx_build(im);
    /* Release: the panic path reads kx only once it sees ARMED. */
    if (st == OK)
        __atomic_store_n(&kx_state, done, __ATOMIC_RELEASE);
    mutex_unlock(&kx_lock);
    return st;
}

void kexec_crash_load(void)
{
    if (__atomic_load_n(&kx_state, __ATOMIC_ACQUIRE) != KX_EMPTY)
        return;
    kx_window_init();
    const struct boot_module *k = module(KEXEC_KERNEL_MODULE), *b = module(BOOTFS_MODULE);
    if (!k || !b) {
        kprintf("kexec: no %s module (boot/limine.conf): no crash kernel\n",
                k ? BOOTFS_MODULE : KEXEC_KERNEL_MODULE);
        return;
    }
    static char cmd[KEXEC_CMDLINE];
    crash_cmdline(cmd, sizeof(cmd));
    struct kx_image im = {
        .kernel = phys_to_virt(k->phys), .kernel_size = k->size,
        .bootfs = phys_to_virt(b->phys), .bootfs_size = b->size,
        .crash = true, .cmdline = cmd,
    };
    if (load(&im, KX_ARMED) == OK)
        kprintf("kexec: crash kernel armed, command line \"%s\"\n", cmd);
    else
        kprintf("kexec: no crash kernel: a panic halts as before\n");
}

bool kexec_crash_armed(void)
{
    return __atomic_load_n(&kx_state, __ATOMIC_ACQUIRE) == KX_ARMED;
}

status_t kexec_load_image(struct vmo *kernel, struct vmo *bootfs, const char *cmdline)
{
    if (__atomic_load_n(&kx_state, __ATOMIC_ACQUIRE) == KX_OFF)
        return ERR_NOT_SUPPORTED;
    if (!cmdline || !cmdline[0])
        cmdline = cmdline_get();
    if (strlen(cmdline) >= KEXEC_CMDLINE)
        return ERR_INVALID_ARGS;
    uint64_t ks = vmo_size(kernel), bs = vmo_size(bootfs);
    if (!ks || !bs || ks > KERNEL_FILE_MAX || bs > BOOTFS_FILE_MAX)
        return ERR_NO_RESOURCES;
    void *kp = NULL, *bp = NULL;
    status_t st = vmo_map_kernel(kernel, 0, ks, 0, &kp);
    if (st == OK)
        st = vmo_map_kernel(bootfs, 0, bs, 0, &bp);
    if (st == OK) {
        struct kx_image im = {
            .kernel = kp, .kernel_size = ks, .bootfs = bp, .bootfs_size = bs,
            .crash = false, .cmdline = cmdline,
        };
        st = load(&im, KX_IMAGE);
    }
    if (bp)
        (void)vmo_unmap_kernel(bootfs, bp);   /* our own mapping: nothing to do if it failed */
    if (kp)
        (void)vmo_unmap_kernel(kernel, kp);
    return st;
}
